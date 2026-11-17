// OsmTiles: street map tiles from OpenStreetMap. See OsmTiles.h.

#include "OsmTiles.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

using json = nlohmann::json;

// Vector tiles (OpenMapTiles schema) from OpenFreeMap: free, no key, served
// from a CDN. Zoom 13 tiles are ~3 km across in London and still carry
// residential streets.
static const char *TILEJSON_URL = "https://tiles.openfreemap.org/planet";
static const int ZOOM = 13;

// tiles kept in memory (a busy city tile is ~100 KB)
static const size_t MAX_TILES = 64;

// failed tiles (offline, server trouble) are retried after this long
static const long long RETRY_MS = 2 * 60 * 1000;

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static size_t appendBody(char *data, size_t size, size_t count, void *out) {
    static_cast<std::string *>(out)->append(data, size * count);
    return size * count;
}

// slippy-map tile maths
static double tileX(double lon) {
    return (lon + 180.0) / 360.0 * (1 << ZOOM);
}

static double tileY(double lat) {
    double r = lat * M_PI / 180.0;
    return (1.0 - asinh(tan(r)) / M_PI) / 2.0 * (1 << ZOOM);
}

static double tileLon(double x) {
    return x / (1 << ZOOM) * 360.0 - 180.0;
}

static double tileLat(double y) {
    return atan(sinh(M_PI * (1.0 - 2.0 * y / (1 << ZOOM)))) * 180.0 / M_PI;
}

OsmTiles::OsmTiles() {
    centerLat = 0;
    centerLon = 0;
    running = true;
    worker = std::thread(&OsmTiles::run, this);
}

OsmTiles::~OsmTiles() {
    {
        std::lock_guard<std::mutex> guard(lock);
        running = false;
    }
    wake.notify_all();
    if(worker.joinable()) {
        worker.join();
    }
    for(auto &kv : tiles) {
        delete kv.second;
    }
    for(auto &d : done) {
        delete d.second;
    }
}

OsmTiles::Key OsmTiles::keyFor(int ty, int tx) {
    return ((int64_t)ty << 32) | (uint32_t)tx;
}

static int keyY(int64_t k) {
    return (int)(k >> 32);
}

static int keyX(int64_t k) {
    return (int)(uint32_t)(k & 0xffffffff);
}

void OsmTiles::tileBounds(Key k, OsmTile *t) {
    t->lonMin = tileLon(keyX(k));
    t->lonMax = tileLon(keyX(k) + 1);
    t->latMax = tileLat(keyY(k));
    t->latMin = tileLat(keyY(k) + 1);
}

void OsmTiles::want(float latMin, float latMax, float lonMin, float lonMax) {
    if(std::isnan(latMin) || std::isnan(latMax) || std::isnan(lonMin) || std::isnan(lonMax)) {
        return;
    }
    centerLat = (latMin + latMax) / 2;
    centerLon = (lonMin + lonMax) / 2;

    int n = 1 << ZOOM;
    int ty0 = std::max(0, (int)floor(tileY(latMax))), ty1 = std::min(n - 1, (int)floor(tileY(latMin)));
    int tx0 = std::max(0, (int)floor(tileX(lonMin))), tx1 = std::min(n - 1, (int)floor(tileX(lonMax)));
    double cx = tileX(centerLon), cy = tileY(centerLat);

    // nearest the centre first
    std::vector<std::pair<double, Key>> missing;
    long long t = nowMs();
    {
        std::lock_guard<std::mutex> guard(lock);
        // the queue is rebuilt from what is on screen now; only the tile
        // being loaded stays pending
        for(Key k : queue) {
            pending.erase(k);
        }
        queue.clear();
        for(int ty = ty0; ty <= ty1; ty++) {
            for(int tx = tx0; tx <= tx1; tx++) {
                Key k = keyFor(ty, tx);
                if(tiles.count(k) || pending.count(k)) {
                    continue;
                }
                auto f = failedAt.find(k);
                if(f != failedAt.end() && t - f->second < RETRY_MS) {
                    continue;
                }
                double dx = tx + 0.5 - cx, dy = ty + 0.5 - cy;
                missing.push_back(std::make_pair(dx * dx + dy * dy, k));
            }
        }
        std::sort(missing.begin(), missing.end());
        for(auto &m : missing) {
            queue.push_back(m.second);
            pending.insert(m.second);
        }
    }
    if(!missing.empty()) {
        wake.notify_one();
    }
}

bool OsmTiles::poll() {
    std::vector<std::pair<Key, OsmTile *>> arrived;
    {
        std::lock_guard<std::mutex> guard(lock);
        arrived.swap(done);
        for(auto &d : arrived) {
            pending.erase(d.first);
        }
    }
    for(auto &d : arrived) {
        auto old = tiles.find(d.first);
        if(old != tiles.end()) {
            delete old->second;
        }
        tiles[d.first] = d.second;
    }

    // drop the tiles farthest from the view
    while(tiles.size() > MAX_TILES) {
        auto farthest = tiles.end();
        double best = -1;
        for(auto it = tiles.begin(); it != tiles.end(); ++it) {
            const OsmTile *t = it->second;
            double dy = (t->latMin + t->latMax) / 2 - centerLat;
            double dx = ((t->lonMin + t->lonMax) / 2 - centerLon) * cos(centerLat * M_PI / 180);
            double d = dx * dx + dy * dy;
            if(d > best) {
                best = d;
                farthest = it;
            }
        }
        delete farthest->second;
        tiles.erase(farthest);
    }
    return !arrived.empty();
}

std::vector<const OsmTile *> OsmTiles::visible(float latMin, float latMax, float lonMin, float lonMax) const {
    std::vector<const OsmTile *> out;
    for(auto &kv : tiles) {
        const OsmTile *t = kv.second;
        if(t->latMin > latMax || t->latMax < latMin || t->lonMin > lonMax || t->lonMax < lonMin) {
            continue;
        }
        out.push_back(t);
    }
    return out;
}

void OsmTiles::run() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    while(true) {
        Key k;
        {
            std::unique_lock<std::mutex> guard(lock);
            wake.wait(guard, [this] { return !running || !queue.empty(); });
            if(!running) {
                return;
            }
            k = queue.front();
            queue.erase(queue.begin());
        }

        OsmTile *t = new OsmTile();
        tileBounds(k, t);
        bool ok = load(k, t);

        std::lock_guard<std::mutex> guard(lock);
        if(ok) {
            done.push_back(std::make_pair(k, t));
        } else {
            delete t;
            pending.erase(k);
            failedAt[k] = nowMs();
        }
    }
}

std::string OsmTiles::cachePath(Key k) {
    const char *home = getenv("HOME");
    std::string dir = std::string(home ? home : "/tmp") + "/.cache";
    mkdir(dir.c_str(), 0755);
    dir += "/viz1090";
    mkdir(dir.c_str(), 0755);
    dir += "/osm";
    mkdir(dir.c_str(), 0755);

    char name[64];
    snprintf(name, sizeof(name), "/v2_%d_%d_%d.bin", ZOOM, keyX(k), keyY(k));
    return dir + name;
}

bool OsmTiles::load(Key k, OsmTile *t) {
    std::string path = cachePath(k);
    if(readCache(path, t)) {
        return true;
    }
    if(!fetch(k, t)) {
        return false;
    }
    writeCache(path, *t);
    return true;
}

// cache file: "OSMT", uint32 version, uint32 point count per layer, floats
bool OsmTiles::readCache(const std::string &path, OsmTile *t) {
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) {
        return false;
    }
    char magic[4];
    uint32_t version = 0, counts[OSM_LAYER_COUNT];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "OSMT", 4) == 0 &&
              fread(&version, 4, 1, f) == 1 && version == 2 &&
              fread(counts, 4, OSM_LAYER_COUNT, f) == OSM_LAYER_COUNT;
    for(int i = 0; ok && i < OSM_LAYER_COUNT; i++) {
        t->pts[i].resize(counts[i]);
        ok = counts[i] == 0 || fread(t->pts[i].data(), sizeof(float), counts[i], f) == counts[i];
    }
    fclose(f);
    return ok;
}

void OsmTiles::writeCache(const std::string &path, const OsmTile &t) {
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if(!f) {
        return;
    }
    uint32_t version = 2, counts[OSM_LAYER_COUNT];
    for(int i = 0; i < OSM_LAYER_COUNT; i++) {
        counts[i] = t.pts[i].size();
    }
    fwrite("OSMT", 1, 4, f);
    fwrite(&version, 4, 1, f);
    fwrite(counts, 4, OSM_LAYER_COUNT, f);
    for(int i = 0; i < OSM_LAYER_COUNT; i++) {
        fwrite(t.pts[i].data(), sizeof(float), t.pts[i].size(), f);
    }
    bool ok = fclose(f) == 0;
    if(ok) {
        rename(tmp.c_str(), path.c_str());
    } else {
        remove(tmp.c_str());
    }
}

static bool httpGet(const std::string &url, std::string *body) {
    CURL *curl = curl_easy_init();
    if(!curl) {
        return false;
    }
    long code = 0;
    char err[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "viz1090");
    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_easy_cleanup(curl);
    if(res != CURLE_OK || code != 200) {
        fprintf(stderr, "map tiles: %s: %ld %s\n", url.c_str(), code, err);
        return false;
    }
    return true;
}

//
// Mapbox Vector Tile decoding: just enough protobuf for the layers we draw.
//

struct PbReader {
    const uint8_t *p, *end;

    PbReader(const uint8_t *b, const uint8_t *e) : p(b), end(e) {}

    bool more() const { return p < end; }

    uint64_t varint() {
        uint64_t v = 0;
        for(int shift = 0; p < end && shift < 64; shift += 7) {
            uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if(!(b & 0x80)) {
                break;
            }
        }
        return v;
    }

    // next field: number and wire type; false at the end
    bool next(int *field, int *wire) {
        if(p >= end) {
            return false;
        }
        uint64_t key = varint();
        *field = (int)(key >> 3);
        *wire = (int)(key & 7);
        return true;
    }

    PbReader sub() {
        uint64_t len = varint();
        const uint8_t *s = p;
        p = std::min(end, p + len);
        return PbReader(s, p);
    }

    void skip(int wire) {
        switch(wire) {
            case 0: varint(); break;
            case 1: p = std::min(end, p + 8); break;
            case 2: sub(); break;
            case 5: p = std::min(end, p + 4); break;
            default: p = end; break;
        }
    }
};

static std::string pbString(PbReader r) {
    return std::string((const char *)r.p, r.end - r.p);
}

// which of our layers a feature goes in, or -1 to drop it
static int classify(const std::string &layer, const std::map<std::string, std::string> &tags) {
    auto tag = [&](const char *k) {
        auto it = tags.find(k);
        return it == tags.end() ? std::string() : it->second;
    };
    if(layer == "water") {
        return OSM_WATER;
    }
    if(layer == "waterway") {
        std::string c = tag("class");
        return (c == "river" || c == "canal") ? OSM_WATER : -1;
    }
    if(layer == "transportation") {
        std::string c = tag("class");
        if(c == "motorway" || c == "trunk" || c == "primary" || c == "secondary" || c == "tertiary") {
            return OSM_ROADS_MAJOR;
        }
        if(c == "minor") {
            return OSM_ROADS_MINOR;
        }
        if((c == "rail" || c == "transit") && tag("brunnel") != "tunnel") {
            return OSM_RAIL;
        }
    }
    return -1;
}

// Add one decoded polyline (tile coordinates) to a layer. Segments running
// along or beyond the tile edge are dropped: those are clipping artefacts of
// polygons, or the buffer the neighbouring tile already draws.
static void addPolyline(const std::vector<std::pair<int, int>> &line, int extent, int tx, int ty, std::vector<float> *pts) {
    bool open = false;
    for(size_t i = 1; i < line.size(); i++) {
        int x1 = line[i - 1].first, y1 = line[i - 1].second;
        int x2 = line[i].first, y2 = line[i].second;
        bool edge = (x1 <= 0 && x2 <= 0) || (x1 >= extent && x2 >= extent) ||
                    (y1 <= 0 && y2 <= 0) || (y1 >= extent && y2 >= extent);
        if(edge) {
            if(open) {
                pts->push_back(NAN);
                pts->push_back(NAN);
                open = false;
            }
            continue;
        }
        if(!open) {
            pts->push_back(tileLon(tx + x1 / (double)extent));
            pts->push_back(tileLat(ty + y1 / (double)extent));
            open = true;
        }
        pts->push_back(tileLon(tx + x2 / (double)extent));
        pts->push_back(tileLat(ty + y2 / (double)extent));
    }
    if(open) {
        pts->push_back(NAN);
        pts->push_back(NAN);
    }
}

static void decodeLayer(PbReader layer, int tx, int ty, OsmTile *t) {
    std::string name;
    std::vector<std::string> keys, values;
    std::vector<PbReader> features;
    int extent = 4096;

    int field, wire;
    while(layer.next(&field, &wire)) {
        if(field == 1 && wire == 2) {
            name = pbString(layer.sub());
            if(name != "water" && name != "waterway" && name != "transportation") {
                return;
            }
        } else if(field == 2 && wire == 2) {
            features.push_back(layer.sub());
        } else if(field == 3 && wire == 2) {
            keys.push_back(pbString(layer.sub()));
        } else if(field == 4 && wire == 2) {
            // only string values matter here (class, brunnel)
            PbReader v = layer.sub();
            std::string s;
            int vf, vw;
            while(v.next(&vf, &vw)) {
                if(vf == 1 && vw == 2) {
                    s = pbString(v.sub());
                } else {
                    v.skip(vw);
                }
            }
            values.push_back(s);
        } else if(field == 5 && wire == 0) {
            extent = (int)layer.varint();
        } else {
            layer.skip(wire);
        }
    }

    for(PbReader f : features) {
        std::map<std::string, std::string> tags;
        PbReader geometry(nullptr, nullptr);
        int type = 0;
        while(f.next(&field, &wire)) {
            if(field == 2 && wire == 2) {
                PbReader tr = f.sub();
                while(tr.more()) {
                    uint64_t ki = tr.varint(), vi = tr.varint();
                    if(ki < keys.size() && vi < values.size()) {
                        tags[keys[ki]] = values[vi];
                    }
                }
            } else if(field == 3 && wire == 0) {
                type = (int)f.varint();
            } else if(field == 4 && wire == 2) {
                geometry = f.sub();
            } else {
                f.skip(wire);
            }
        }
        if(type != 2 && type != 3) {
            continue;  // points
        }
        int out = classify(name, tags);
        if(out < 0) {
            continue;
        }

        // geometry commands: MoveTo(1) / LineTo(2) / ClosePath(7), zigzag deltas
        std::vector<std::pair<int, int>> line;
        int x = 0, y = 0;
        while(geometry.more()) {
            uint32_t cmd = (uint32_t)geometry.varint();
            int id = cmd & 7, count = cmd >> 3;
            if(id == 7) {
                if(!line.empty()) {
                    line.push_back(line.front());
                }
                continue;
            }
            for(int i = 0; i < count && geometry.more(); i++) {
                uint32_t zx = (uint32_t)geometry.varint(), zy = (uint32_t)geometry.varint();
                x += (int)((zx >> 1) ^ -(zx & 1));
                y += (int)((zy >> 1) ^ -(zy & 1));
                if(id == 1) {
                    addPolyline(line, extent, tx, ty, &t->pts[out]);
                    line.clear();
                }
                line.push_back(std::make_pair(x, y));
            }
        }
        addPolyline(line, extent, tx, ty, &t->pts[out]);
    }
}

bool OsmTiles::fetch(Key k, OsmTile *t) {
    if(urlTemplate.empty()) {
        std::string body;
        if(!httpGet(TILEJSON_URL, &body)) {
            return false;
        }
        try {
            urlTemplate = json::parse(body).at("tiles").at(0).get<std::string>();
        } catch(const std::exception &) {
            return false;
        }
    }

    std::string url = urlTemplate;
    auto put = [&](const char *name, int v) {
        size_t at = url.find(name);
        if(at != std::string::npos) {
            url.replace(at, strlen(name), std::to_string(v));
        }
    };
    put("{z}", ZOOM);
    put("{x}", keyX(k));
    put("{y}", keyY(k));

    std::string body;
    if(!httpGet(url, &body)) {
        urlTemplate.clear();  // the planet build may have moved on: re-read the TileJSON next time
        return false;
    }

    PbReader tile((const uint8_t *)body.data(), (const uint8_t *)body.data() + body.size());
    int field, wire;
    while(tile.next(&field, &wire)) {
        if(field == 3 && wire == 2) {
            decodeLayer(tile.sub(), keyX(k), keyY(k), t);
        } else {
            tile.skip(wire);
        }
    }
    return true;
}
