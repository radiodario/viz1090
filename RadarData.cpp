// RadarData: latest RainViewer radar tiles around a point. See RadarData.h.

#include "RadarData.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using json = nlohmann::json;

static const int REFRESH_S = 300;
static const int RETRY_S = 60;
static const int ZOOM = 7;          // highest zoom RainViewer's free tier serves
static const int GRID = 1;          // tiles either side of home: 3x3

static size_t appendBytes(char *data, size_t size, size_t count, void *out) {
    std::vector<unsigned char> *buf = static_cast<std::vector<unsigned char> *>(out);
    buf->insert(buf->end(), (unsigned char *)data, (unsigned char *)data + size * count);
    return size * count;
}

static bool download(const std::string &url, std::vector<unsigned char> *out) {
    CURL *curl = curl_easy_init();
    if(!curl) {
        return false;
    }
    long code = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "viz1090-radar");
    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_easy_cleanup(curl);
    return res == CURLE_OK && code == 200;
}

// slippy-map tile maths (Web Mercator)
static double tileLon(int x, int z) {
    return x / pow(2.0, z) * 360.0 - 180.0;
}

static double tileLat(int y, int z) {
    double n = M_PI - 2.0 * M_PI * y / pow(2.0, z);
    return 180.0 / M_PI * atan(sinh(n));
}

RadarData::RadarData() {
    lat = 0;
    lon = 0;
    running = false;
}

RadarData::~RadarData() {
    running = false;
    if(worker.joinable()) {
        worker.join();
    }
}

void RadarData::start(double lat, double lon) {
    this->lat = lat;
    this->lon = lon;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    running = true;
    worker = std::thread(&RadarData::run, this);
}

void RadarData::run() {
    while(running) {
        int wait = fetch() ? REFRESH_S : RETRY_S;
        for(int i = 0; i < wait * 4 && running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

bool RadarData::fetch() {
    std::vector<unsigned char> index;
    if(!download("https://api.rainviewer.com/public/weather-maps.json", &index)) {
        return false;
    }

    std::string host, path;
    time_t frameTime = 0;
    try {
        json doc = json::parse(index.begin(), index.end());
        host = doc.value("host", "");
        const json &past = doc.at("radar").at("past");
        if(past.empty()) {
            return false;
        }
        path = past.back().value("path", "");
        frameTime = past.back().value("time", (time_t)0);
    } catch(const std::exception &) {
        return false;
    }

    if(path == lastPath) {
        return true;  // nothing new yet
    }

    int n = 1 << ZOOM;
    int cx = (int)floor((lon + 180.0) / 360.0 * n);
    double latRad = lat * M_PI / 180.0;
    int cy = (int)floor((1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n);

    RadarFrame frame;
    frame.time = frameTime;
    for(int dy = -GRID; dy <= GRID; dy++) {
        for(int dx = -GRID; dx <= GRID; dx++) {
            if(!running) {
                return false;
            }
            RadarTile tile;
            tile.z = ZOOM;
            tile.x = (cx + dx + n) % n;
            tile.y = cy + dy;
            if(tile.y < 0 || tile.y >= n) {
                continue;
            }
            tile.lonLeft = tileLon(tile.x, ZOOM);
            tile.lonRight = tileLon(tile.x + 1, ZOOM);
            tile.latTop = tileLat(tile.y, ZOOM);
            tile.latBottom = tileLat(tile.y + 1, ZOOM);

            // colour scheme 2 (universal blue), unsmoothed for crisp pixels, with snow
            char url[512];
            snprintf(url, sizeof(url), "%s%s/256/%d/%d/%d/2/0_1.png", host.c_str(), path.c_str(), ZOOM, tile.x, tile.y);
            if(download(url, &tile.png)) {
                frame.tiles.push_back(tile);
            }
        }
    }

    if(frame.tiles.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> guard(lock);
    frame.version = current.version + 1;
    current = frame;
    lastPath = path;
    return true;
}

RadarFrame RadarData::frame() {
    std::lock_guard<std::mutex> guard(lock);
    return current;
}

int RadarData::version() {
    std::lock_guard<std::mutex> guard(lock);
    return current.version;
}
