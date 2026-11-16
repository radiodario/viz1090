// RadarData: on-demand RainViewer radar tiles for every recent frame.
// See RadarData.h.

#include "RadarData.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static const int FRAMES_REFRESH_S = 300;
static const int WANT_EXPIRY_S = 120;   // stop fetching tiles nobody has looked at for this long

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

// key() is "<path>/z/x/y": strip the last three parts to get the frame path
static std::string pathOfKey(const std::string &k) {
    size_t cut = k.size();
    for(int i = 0; i < 3 && cut != std::string::npos; i++) {
        cut = k.rfind('/', cut - 1);
    }
    return cut == std::string::npos ? "" : k.substr(0, cut);
}

std::string RadarData::key(const std::string &path, int z, int x, int y) {
    char k[160];
    snprintf(k, sizeof(k), "%s/%d/%d/%d", path.c_str(), z, x, y);
    return k;
}

RadarData::RadarData() {
    running = false;
    changes = 0;
}

RadarData::~RadarData() {
    running = false;
    if(worker.joinable()) {
        worker.join();
    }
}

void RadarData::start() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    running = true;
    worker = std::thread(&RadarData::run, this);
}

void RadarData::run() {
    Clock::time_point lastFrames = Clock::now() - std::chrono::seconds(FRAMES_REFRESH_S);
    while(running) {
        if(Clock::now() - lastFrames >= std::chrono::seconds(FRAMES_REFRESH_S)) {
            if(refreshFrames()) {
                lastFrames = Clock::now();
            } else {
                lastFrames = Clock::now() - std::chrono::seconds(FRAMES_REFRESH_S - 30);  // retry soon
            }
        }
        if(!downloadNext()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

bool RadarData::refreshFrames() {
    std::vector<unsigned char> index;
    if(!download("https://api.rainviewer.com/public/weather-maps.json", &index)) {
        return false;
    }

    std::vector<RadarFrameInfo> list;
    std::string newHost;
    try {
        json doc = json::parse(index.begin(), index.end());
        newHost = doc.value("host", "");
        for(const auto &f : doc.at("radar").at("past")) {
            RadarFrameInfo info;
            info.path = f.value("path", "");
            info.time = f.value("time", (time_t)0);
            list.push_back(info);
        }
    } catch(const std::exception &) {
        return false;
    }
    if(list.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> guard(lock);
    host = newHost;
    frameList = list;

    // drop tiles from frames that have rolled off
    std::set<std::string> paths;
    for(const auto &f : frameList) {
        paths.insert(f.path);
    }
    for(auto it = tiles.begin(); it != tiles.end();) {
        std::string path = pathOfKey(it->first);
        if(!paths.count(path)) {
            it = tiles.erase(it);
        } else {
            ++it;
        }
    }
    changes++;
    return true;
}

// Download one missing tile: newest frame first, so the current picture
// appears before the loop fills in. Returns false when there's nothing to do.
bool RadarData::downloadNext() {
    std::string url, k;
    {
        std::lock_guard<std::mutex> guard(lock);
        Clock::time_point now = Clock::now();
        for(auto it = wanted.begin(); it != wanted.end();) {
            if(now - it->second > std::chrono::seconds(WANT_EXPIRY_S)) {
                it = wanted.erase(it);
            } else {
                ++it;
            }
        }
        for(auto f = frameList.rbegin(); f != frameList.rend() && url.empty(); ++f) {
            for(const auto &w : wanted) {
                int z, x, y;
                if(sscanf(w.first.c_str(), "%d/%d/%d", &z, &x, &y) != 3) {
                    continue;
                }
                std::string candidate = key(f->path, z, x, y);
                if(tiles.count(candidate) || failed.count(candidate)) {
                    continue;
                }
                k = candidate;
                // colour scheme 2 (universal blue), unsmoothed for crisp pixels, with snow
                char u[512];
                snprintf(u, sizeof(u), "%s%s/256/%d/%d/%d/2/0_1.png", host.c_str(), f->path.c_str(), z, x, y);
                url = u;
                break;
            }
        }
    }
    if(url.empty()) {
        return false;
    }

    std::vector<unsigned char> png;
    bool ok = download(url, &png);

    std::lock_guard<std::mutex> guard(lock);
    if(ok) {
        tiles[k] = png;
        changes++;
    } else {
        failed.insert(k);
    }
    return true;
}

std::vector<RadarFrameInfo> RadarData::frames() {
    std::lock_guard<std::mutex> guard(lock);
    return frameList;
}

void RadarData::want(int z, int x, int y) {
    char k[64];
    snprintf(k, sizeof(k), "%d/%d/%d", z, x, y);
    std::lock_guard<std::mutex> guard(lock);
    wanted[k] = Clock::now();
}

bool RadarData::tile(const std::string &path, int z, int x, int y, std::vector<unsigned char> *png) {
    std::lock_guard<std::mutex> guard(lock);
    auto it = tiles.find(key(path, z, x, y));
    if(it == tiles.end()) {
        return false;
    }
    *png = it->second;
    return true;
}

int RadarData::version() {
    std::lock_guard<std::mutex> guard(lock);
    return changes;
}
