// WeatherData: local forecast from Open-Meteo (open-meteo.com, no key needed)
// for viz1090's weather page. A background thread refreshes every 10 minutes.

#ifndef WEATHERDATA_H
#define WEATHERDATA_H

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct WeatherSlot {
    time_t time;
    float temp;           // C
    int code;             // WMO weather code
    int precipProb;       // %, -1 if unknown
    float precip;         // mm
    bool isDay;
};

struct WeatherDay {
    time_t date;
    int code;
    float tempMax;
    float tempMin;
    int precipProb;
};

struct WeatherSnapshot {
    bool valid = false;
    // now
    float temp = 0;
    float feelsLike = 0;
    float windMph = 0;
    int windDir = 0;      // degrees, where the wind comes from
    int humidity = 0;
    int code = 0;
    bool isDay = true;
    std::vector<WeatherSlot> quarterHours;  // next hour, 15-minute steps
    std::vector<WeatherSlot> hours;         // next 48 hours
    std::vector<WeatherDay> days;           // next 7 days
    time_t fetched = 0;
};

class WeatherData {
public:
    WeatherData();
    ~WeatherData();

    void start(double lat, double lon);
    WeatherSnapshot snapshot();
    std::string status();

private:
    void run();
    bool fetch();

    double lat;
    double lon;
    std::thread worker;
    std::atomic<bool> running;
    std::mutex lock;
    WeatherSnapshot current;
    std::string lastError;
};

// WMO weather code -> short description, e.g. "Light rain"
std::string weatherDescription(int code);

#endif
