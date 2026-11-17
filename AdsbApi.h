// AdsbApi: aircraft from an online ADS-B aggregator instead of a local
// dump1090. Both feeds speak ADS-B Exchange's v2 JSON:
//   adsb.fi  (default) opendata.adsb.fi, ~1 request/s allowed, no daily cap
//   adsb.lol           api.adsb.lol, only 500 requests a day: polled slowly
//
// A background thread polls while a map page is showing (setActive); the
// main loop copies the latest batch into the AircraftList the map draws.

#ifndef ADSBAPI_H
#define ADSBAPI_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ApiAircraft {
    uint32_t addr;      // ICAO hex
    char flight[16];
    float lat, lon;
    int altitude;       // feet; 0 on the ground
    int speed;          // knots
    int track;          // degrees
    int vertRate;       // feet/min
};

class AdsbApi {
public:
    AdsbApi();
    ~AdsbApi();

    // centre and radius (nautical miles) of the area to fetch
    // feed: "adsb.fi" or "adsb.lol"
    void start(const std::string &feed, double lat, double lon, int radiusNm);

    // poll only while someone can see the planes
    void setActive(bool active);

    // latest batch; returns false if nothing new since the last call
    bool take(std::vector<ApiAircraft> *out);

private:
    void run();
    bool fetch();

    std::string url;
    int pollMs;
    std::atomic<bool> active;
    std::thread worker;
    std::atomic<bool> running;
    std::mutex lock;
    std::vector<ApiAircraft> latest;
    bool fresh;
};

#endif
