// TransitBoard: live TfL departures near a location. See TransitBoard.h.

#include "TransitBoard.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sys/stat.h>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static const char *TFL_BASE = "https://api.tfl.gov.uk";

static const int ARRIVALS_INTERVAL_S = 30;
static const int STOPS_RETRY_S = 15;
static const size_t ARRIVALS_KEPT = 5;  // per row, so countdowns keep the row populated
static const size_t MARKER_ARRIVALS_KEPT = 4;
static const int WALK_LOOKUPS_PER_CYCLE = 10;  // spread journey-planner calls out
static const int MAX_WALK_MINUTES = 15;  // stops further than this on foot are ignored

// Walking estimate: straight-line distance plus 25% for real streets, at ~80 m/min.
static int walkMinutesFor(double meters) {
    return (int)std::ceil(meters * 1.25 / 80.0);
}

// "N", "NE", ... -> degrees from north, or -1
static float compassToDegrees(const std::string &point) {
    const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    for(int i = 0; i < 8; i++) {
        if(point == names[i]) {
            return i * 45.0f;
        }
    }
    return -1.0f;
}

static bool isTransitMode(const std::string &mode) {
    // national-rail is not covered by TfL arrivals, so leave it out for now
    return mode == "bus" || mode == "overground" || mode == "tube" ||
           mode == "elizabeth-line" || mode == "dlr" || mode == "tram";
}

static std::string tidyDestination(std::string name) {
    const char *suffixes[] = {" Rail Station", " Underground Station", " DLR Station", " Bus Station"};
    for(const char *suffix : suffixes) {
        std::string s(suffix);
        if(name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
            name.erase(name.size() - s.size());
        }
    }
    return name;
}

static size_t appendBody(char *data, size_t size, size_t count, void *out) {
    static_cast<std::string *>(out)->append(data, size * count);
    return size * count;
}

TransitBoard::TransitBoard() {
    lat = 0;
    lon = 0;
    radiusMeters = 1000;
    running = false;
    haveUpdate = false;
}

TransitBoard::~TransitBoard() {
    running = false;
    if(worker.joinable()) {
        worker.join();
    }
}

void TransitBoard::start(double lat, double lon, int radiusMeters) {
    this->lat = lat;
    this->lon = lon;
    this->radiusMeters = radiusMeters;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    running = true;
    worker = std::thread(&TransitBoard::run, this);
}

void TransitBoard::run() {
    // sleep in short steps so shutdown is quick
    auto sleepFor = [this](int seconds) {
        for(int i = 0; i < seconds * 4 && running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    };

    while(running && !fetchStops()) {
        sleepFor(STOPS_RETRY_S);
    }
    loadWalkCache();

    while(running) {
        fetchArrivals();
        refineWalkTimes();
        sleepFor(ARRIVALS_INTERVAL_S);
    }
}

// Optional TfL app key raises rate limits: export TFL_APP_KEY=...
std::string TransitBoard::withKey(const std::string &url) {
    const char *key = getenv("TFL_APP_KEY");
    if(!key || !*key) {
        return url;
    }
    return url + (url.find('?') == std::string::npos ? "?" : "&") + "app_key=" + key;
}

std::string TransitBoard::httpGet(const std::string &url, long *httpCode) {
    std::string body;
    *httpCode = 0;

    CURL *curl = curl_easy_init();
    if(!curl) {
        return body;
    }

    curl_easy_setopt(curl, CURLOPT_URL, withKey(url).c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // accept gzip
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "viz1090-transit");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCode);
    } else {
        std::lock_guard<std::mutex> guard(lock);
        lastError = curl_easy_strerror(res);
    }
    curl_easy_cleanup(curl);
    return body;
}

bool TransitBoard::fetchStops() {
    char url[512];
    snprintf(url, sizeof(url),
             "%s/StopPoint?lat=%.6f&lon=%.6f&radius=%d"
             "&stopTypes=NaptanPublicBusCoachTram,NaptanRailStation,NaptanMetroStation",
             TFL_BASE, lat, lon, radiusMeters);

    long code;
    std::string body = httpGet(url, &code);
    if(code != 200) {
        std::lock_guard<std::mutex> guard(lock);
        if(code) {
            lastError = "stops: HTTP " + std::to_string(code);
        }
        return false;
    }

    std::vector<Stop> found;
    std::set<std::string> lines;

    try {
        json doc = json::parse(body);
        for(const auto &sp : doc.at("stopPoints")) {
            Stop stop;
            stop.id = sp.value("naptanId", "");
            stop.name = sp.value("commonName", "");
            stop.indicator = sp.value("indicator", "");
            stop.walkMinutes = walkMinutesFor(sp.value("distance", 0.0));
            stop.walkRouted = false;
            stop.station = sp.value("stopType", "") != "NaptanPublicBusCoachTram";
            stop.lat = sp.value("lat", 0.0);
            stop.lon = sp.value("lon", 0.0);
            stop.heading = -1.0f;
            if(sp.contains("additionalProperties")) {
                for(const auto &prop : sp["additionalProperties"]) {
                    if(prop.value("key", "") == "CompassPoint") {
                        stop.heading = compassToDegrees(prop.value("value", ""));
                    }
                }
            }

            bool served = false;
            if(sp.contains("lineModeGroups")) {
                for(const auto &group : sp["lineModeGroups"]) {
                    if(!isTransitMode(group.value("modeName", ""))) {
                        continue;
                    }
                    for(const auto &lineId : group.at("lineIdentifier")) {
                        lines.insert(lineId.get<std::string>());
                        served = true;
                    }
                }
            }

            if(served && !stop.id.empty()) {
                found.push_back(stop);
            }
        }
    } catch(const std::exception &e) {
        std::lock_guard<std::mutex> guard(lock);
        lastError = std::string("stops: ") + e.what();
        return false;
    }

    std::lock_guard<std::mutex> guard(lock);
    stops = found;
    lineIds.assign(lines.begin(), lines.end());
    lastError = found.empty() ? "no stops found" : "";
    return !found.empty();
}

bool TransitBoard::fetchArrivals() {
    std::vector<Stop> stopsCopy;
    std::string lineList;
    {
        std::lock_guard<std::mutex> guard(lock);
        stopsCopy = stops;
        for(const auto &id : lineIds) {
            lineList += (lineList.empty() ? "" : ",") + id;
        }
    }

    std::map<std::string, const Stop *> stopById;
    for(const auto &stop : stopsCopy) {
        stopById[stop.id] = &stop;
    }

    long code;
    std::string body = httpGet(std::string(TFL_BASE) + "/Line/" + lineList + "/Arrivals", &code);
    Clock::time_point fetchedAt = Clock::now();

    if(code != 200) {
        std::lock_guard<std::mutex> guard(lock);
        if(code) {
            lastError = "arrivals: HTTP " + std::to_string(code);
        }
        return false;
    }

    // line|destination -> row; keep the nearest stop with a catchable arrival
    std::map<std::string, Departure> rows;
    // stop id -> upcoming (line, arrival), for the map
    std::map<std::string, std::vector<std::pair<std::string, Clock::time_point>>> byStop;

    try {
        json doc = json::parse(body);
        for(const auto &a : doc) {
            auto it = stopById.find(a.value("naptanId", ""));
            if(it == stopById.end()) {
                continue;
            }
            const Stop *stop = it->second;
            if(stop->walkMinutes > MAX_WALK_MINUTES) {
                continue;  // inside the radius as the crow flies, but too far on foot
            }

            int seconds = a.value("timeToStation", 0);
            byStop[stop->id].push_back(std::make_pair(a.value("lineName", ""), fetchedAt + std::chrono::seconds(seconds)));

            if(seconds < stop->walkMinutes * 60) {
                continue;  // can't get there in time
            }

            std::string line = a.value("lineName", "");
            std::string destination = tidyDestination(a.value("destinationName", ""));
            std::string key = line + "|" + destination;

            auto row = rows.find(key);
            if(row == rows.end() || stop->walkMinutes < row->second.walkMinutes) {
                Departure dep;
                dep.line = line;
                dep.destination = destination;
                dep.stopName = stop->name;
                dep.stopIndicator = stop->indicator;
                dep.mode = a.value("modeName", "");
                dep.walkMinutes = stop->walkMinutes;
                rows[key] = dep;
                row = rows.find(key);
            } else if(stop->walkMinutes > row->second.walkMinutes || stop->name != row->second.stopName) {
                continue;  // a nearer stop already covers this line and direction
            }

            row->second.arrivals.push_back(fetchedAt + std::chrono::seconds(seconds));
        }
    } catch(const std::exception &e) {
        std::lock_guard<std::mutex> guard(lock);
        lastError = std::string("arrivals: ") + e.what();
        return false;
    }

    std::vector<Departure> board;
    for(auto &kv : rows) {
        Departure &dep = kv.second;
        std::sort(dep.arrivals.begin(), dep.arrivals.end());
        if(dep.arrivals.size() > ARRIVALS_KEPT) {
            dep.arrivals.resize(ARRIVALS_KEPT);
        }
        board.push_back(dep);
    }

    std::vector<StopMarker> stopList;
    for(const auto &stop : stopsCopy) {
        StopMarker m;
        m.name = stop.name;
        m.indicator = stop.indicator;
        m.station = stop.station;
        m.heading = stop.heading;
        m.lat = stop.lat;
        m.lon = stop.lon;
        m.walkMinutes = stop.walkMinutes;
        auto it = byStop.find(stop.id);
        if(it != byStop.end()) {
            m.next = it->second;
            std::sort(m.next.begin(), m.next.end(),
                      [](const std::pair<std::string, Clock::time_point> &a,
                         const std::pair<std::string, Clock::time_point> &b) { return a.second < b.second; });
            if(m.next.size() > MARKER_ARRIVALS_KEPT) {
                m.next.resize(MARKER_ARRIVALS_KEPT);
            }
        }
        stopList.push_back(m);
    }

    std::lock_guard<std::mutex> guard(lock);
    markers = stopList;
    departures = board;
    lastUpdate = fetchedAt;
    haveUpdate = true;
    lastError = "";
    return true;
}

//
// Walking times: replace straight-line estimates with TfL walking routes.
//

std::string TransitBoard::walkCachePath() {
    const char *home = getenv("HOME");
    std::string dir = std::string(home ? home : "/tmp") + "/.cache";
    mkdir(dir.c_str(), 0755);
    dir += "/viz1090";
    mkdir(dir.c_str(), 0755);

    char name[96];
    snprintf(name, sizeof(name), "/walk-%.5f_%.5f.txt", lat, lon);
    return dir + name;
}

void TransitBoard::loadWalkCache() {
    FILE *f = fopen(walkCachePath().c_str(), "r");
    if(!f) {
        return;
    }

    std::map<std::string, int> cached;
    char id[64];
    int minutes;
    while(fscanf(f, "%63s %d", id, &minutes) == 2) {
        cached[id] = minutes;
    }
    fclose(f);

    std::lock_guard<std::mutex> guard(lock);
    for(auto &stop : stops) {
        auto it = cached.find(stop.id);
        if(it != cached.end()) {
            stop.walkMinutes = it->second;
            stop.walkRouted = true;
        }
    }
}

void TransitBoard::saveWalkCache() {
    std::string path = walkCachePath();
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if(!f) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        for(const auto &stop : stops) {
            if(stop.walkRouted) {
                fprintf(f, "%s %d\n", stop.id.c_str(), stop.walkMinutes);
            }
        }
    }
    fclose(f);
    rename(tmp.c_str(), path.c_str());
}

void TransitBoard::refineWalkTimes() {
    std::vector<Stop> todo;
    {
        std::lock_guard<std::mutex> guard(lock);
        for(const auto &stop : stops) {
            if(!stop.walkRouted && (int)todo.size() < WALK_LOOKUPS_PER_CYCLE) {
                todo.push_back(stop);
            }
        }
    }
    if(todo.empty()) {
        return;
    }

    bool changed = false;
    for(const auto &stop : todo) {
        if(!running) {
            break;
        }

        char url[256];
        snprintf(url, sizeof(url), "%s/Journey/JourneyResults/%.6f,%.6f/to/%.6f,%.6f?mode=walking",
                 TFL_BASE, lat, lon, stop.lat, stop.lon);

        long code;
        std::string body = httpGet(url, &code);
        if(code == 200) {
            try {
                json doc = json::parse(body);
                int best = -1;
                for(const auto &j : doc.at("journeys")) {
                    int d = j.value("duration", -1);
                    if(d > 0 && (best < 0 || d < best)) {
                        best = d;
                    }
                }
                if(best > 0) {
                    std::lock_guard<std::mutex> guard(lock);
                    for(auto &s : stops) {
                        if(s.id == stop.id) {
                            s.walkMinutes = best;
                            s.walkRouted = true;
                            changed = true;
                        }
                    }
                }
            } catch(const std::exception &) {
                // leave the estimate in place and try again next cycle
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if(changed) {
        saveWalkCache();
    }
}

std::vector<StopMarker> TransitBoard::stopMarkers() {
    std::lock_guard<std::mutex> guard(lock);
    return markers;
}

std::vector<Departure> TransitBoard::snapshot() {
    std::vector<Departure> board;
    {
        std::lock_guard<std::mutex> guard(lock);
        board = departures;
    }

    // drop arrivals that can no longer be caught, then rows with none left
    Clock::time_point now = Clock::now();
    std::vector<Departure> live;
    for(auto &dep : board) {
        Clock::time_point leaveBy = now + std::chrono::minutes(dep.walkMinutes);
        dep.arrivals.erase(std::remove_if(dep.arrivals.begin(), dep.arrivals.end(),
                                          [&](Clock::time_point t) { return t < leaveBy; }),
                           dep.arrivals.end());
        if(!dep.arrivals.empty()) {
            live.push_back(dep);
        }
    }

    std::sort(live.begin(), live.end(), [](const Departure &a, const Departure &b) {
        return a.arrivals.front() < b.arrivals.front();
    });
    return live;
}

std::string TransitBoard::status() {
    std::lock_guard<std::mutex> guard(lock);

    if(!lastError.empty() && !haveUpdate) {
        return lastError;
    }
    if(stops.empty()) {
        return "finding stops";
    }
    if(!haveUpdate) {
        return std::to_string(stops.size()) + " stops, loading";
    }

    long age = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - lastUpdate).count();
    size_t routed = 0;
    for(const auto &stop : stops) {
        routed += stop.walkRouted ? 1 : 0;
    }
    std::string s = std::to_string(stops.size()) + " stops, " + std::to_string(age) + "s ago";
    if(routed < stops.size()) {
        s += ", walks " + std::to_string(routed) + "/" + std::to_string(stops.size());
    }
    if(!lastError.empty()) {
        s += " (" + lastError + ")";
    }
    return s;
}
