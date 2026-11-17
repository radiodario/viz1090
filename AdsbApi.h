// AdsbApi: aircraft from an online ADS-B aggregator instead of (or as well
// as) a local dump1090. Defaults to adsb.lol (open data, no key), whose
// /v2/point API matches ADS-B Exchange's v2 format, so airplanes.live or
// adsb.fi also work with --adsb-api <base url>.
//
// A background thread polls every couple of seconds; the main loop copies
// the latest batch into the AircraftList the map already draws.

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
    void start(const std::string &baseUrl, double lat, double lon, int radiusNm);

    // latest batch; returns false if nothing new since the last call
    bool take(std::vector<ApiAircraft> *out);

private:
    void run();
    bool fetch();

    std::string url;
    std::thread worker;
    std::atomic<bool> running;
    std::mutex lock;
    std::vector<ApiAircraft> latest;
    bool fresh;
};

#endif
