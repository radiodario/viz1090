// AdsbApi: aircraft from an online aggregator. See AdsbApi.h.

#include "AdsbApi.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>

using json = nlohmann::json;


static size_t appendBody(char *data, size_t size, size_t count, void *out) {
    static_cast<std::string *>(out)->append(data, size * count);
    return size * count;
}

AdsbApi::AdsbApi() {
    running = false;
    active = true;
    fresh = false;
    pollMs = 3000;
}

AdsbApi::~AdsbApi() {
    running = false;
    if(worker.joinable()) {
        worker.join();
    }
}

void AdsbApi::start(const std::string &feed, double lat, double lon, int radiusNm) {
    char u[512];
    if(feed == "adsb.lol") {
        snprintf(u, sizeof(u), "https://api.adsb.lol/v2/point/%.3f/%.3f/%d", lat, lon, radiusNm);
        pollMs = 180000;  // 500 requests a day
    } else {
        snprintf(u, sizeof(u), "https://opendata.adsb.fi/api/v2/lat/%.3f/lon/%.3f/dist/%d", lat, lon, radiusNm);
        pollMs = 3000;
    }
    url = u;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    running = true;
    worker = std::thread(&AdsbApi::run, this);
}

void AdsbApi::setActive(bool a) {
    active = a;
}

void AdsbApi::run() {
    int backoffMs = 0;
    while(running) {
        auto started = std::chrono::steady_clock::now();
        int wait = pollMs;
        if(active) {
            if(fetch()) {
                backoffMs = 0;
            } else {
                // errors and rate limits: back off, up to 10 minutes
                backoffMs = backoffMs ? std::min(600000, backoffMs * 2) : 15000;
                wait = backoffMs;
            }
        } else {
            wait = 250;  // idle until a map page shows
        }
        while(running && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(wait)) {
            if(!active && wait > 250) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

bool AdsbApi::fetch() {
    std::string body;
    long code = 0;
    CURL *curl = curl_easy_init();
    if(!curl) {
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "viz1090");
    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_easy_cleanup(curl);
    if(res != CURLE_OK || code != 200) {
        return false;
    }

    std::vector<ApiAircraft> list;
    try {
        json doc = json::parse(body);
        const json &list_ = doc.contains("ac") ? doc["ac"] : doc.at("aircraft");  // adsb.lol / adsb.fi
        for(const json &a : list_) {
            if(!a.contains("lat") || !a.contains("lon") || !a.contains("hex")) {
                continue;
            }
            // positions older than a minute are just noise on a live map
            if(a.value("seen_pos", 0.0) > 60.0) {
                continue;
            }
            ApiAircraft p = {};
            std::string hex = a.value("hex", "");
            if(hex.empty() || hex[0] == '~') {
                continue;  // non-ICAO (TIS-B) ids
            }
            p.addr = (uint32_t)strtoul(hex.c_str(), NULL, 16);
            std::string flight = a.value("flight", "");
            strncpy(p.flight, flight.c_str(), sizeof(p.flight) - 1);
            p.lat = a.value("lat", 0.0f);
            p.lon = a.value("lon", 0.0f);
            const json &alt = a.contains("alt_baro") ? a["alt_baro"] : json();
            p.altitude = alt.is_number() ? alt.get<int>() : 0;  // "ground" -> 0
            p.speed = (int)a.value("gs", 0.0);
            p.track = (int)a.value("track", 0.0);
            p.vertRate = a.value("baro_rate", 0);
            list.push_back(p);
        }
    } catch(const std::exception &) {
        return false;
    }

    std::lock_guard<std::mutex> guard(lock);
    latest.swap(list);
    fresh = true;
    return true;
}

bool AdsbApi::take(std::vector<ApiAircraft> *out) {
    std::lock_guard<std::mutex> guard(lock);
    if(!fresh) {
        return false;
    }
    *out = latest;
    fresh = false;
    return true;
}
