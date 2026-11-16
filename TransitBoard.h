// TransitBoard: live TfL departures near a location, for viz1090's transit page.
//
// A background thread finds every bus stop and TfL rail station within a
// walking radius, then polls arrivals for all lines serving them in one
// request. Arrivals are grouped by line and destination, keeping the nearest
// stop for each, and only arrivals you can still walk to in time are shown.
//
// Walking times start as an estimate from straight-line distance, then are
// replaced by TfL journey-planner walking routes (cached on disk).

#ifndef TRANSITBOARD_H
#define TRANSITBOARD_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Departure {
    std::string line;
    std::string destination;
    std::string stopName;
    std::string stopIndicator;  // e.g. "Stop ND", empty for stations
    std::string mode;           // "bus", "overground", "tube", ...
    int walkMinutes;
    // absolute arrival times, so the board counts down between fetches
    std::vector<std::chrono::steady_clock::time_point> arrivals;
};

// A stop for the map, with its next arrivals across all lines.
struct StopMarker {
    std::string name;
    std::string indicator;
    bool station;
    double lat;
    double lon;
    float heading;   // direction buses leave the stop, degrees from north; -1 if unknown
    std::string railMode;  // "overground", "tube", ... for stations; empty for bus stops
    int walkMinutes;
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> next;  // (line, arrival)
};

class TransitBoard {
public:
    TransitBoard();
    ~TransitBoard();

    void start(double lat, double lon, int radiusMeters);

    // Thread-safe copy of the current board, sorted by next catchable arrival.
    std::vector<Departure> snapshot();

    // Thread-safe copy of the stops with their upcoming arrivals.
    std::vector<StopMarker> stopMarkers();

    // Short human-readable state, e.g. "54 stops, updated 12s ago".
    std::string status();

    double lat;
    double lon;
    int radiusMeters;

private:
    struct Stop {
        std::string id;
        std::string name;
        std::string indicator;
        bool station;
        double lat;
        double lon;
        float heading;
        std::string railMode;
        int walkMinutes;
        bool walkRouted;  // true once walkMinutes comes from a real walking route
    };

    void run();
    bool fetchStops();
    bool fetchArrivals();
    void refineWalkTimes();
    std::string walkCachePath();
    void loadWalkCache();
    void saveWalkCache();
    std::string httpGet(const std::string &url, long *httpCode);
    std::string withKey(const std::string &url);

    std::thread worker;
    std::atomic<bool> running;

    std::mutex lock;
    std::vector<Stop> stops;
    std::vector<std::string> lineIds;
    std::vector<Departure> departures;
    std::vector<StopMarker> markers;
    std::string lastError;
    std::chrono::steady_clock::time_point lastUpdate;
    bool haveUpdate;
};

#endif
