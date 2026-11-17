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
    float windMph = 0;
    int windDir = 0;
};

// a city on the regional map
struct WeatherCity {
    std::string name;
    double lat;
    double lon;
    float temp;
    int code;
    bool isDay;
};

struct WeatherDay {
    time_t date;
    int code;
    float tempMax;
    float tempMin;
    int precipProb;
    time_t sunrise;
    time_t sunset;
    float windMph = 0;    // strongest of the day
    int windDir = 0;      // dominant direction, degrees
};

struct WeatherSnapshot {
    bool valid = false;
    // now
    float temp = 0;
    float feelsLike = 0;
    float windMph = 0;
    int windDir = 0;      // degrees, where the wind comes from
    int humidity = 0;
    float dewPoint = 0;   // C
    float pressure = 0;   // hPa, sea level
    float visibility = 0; // m
    int cloudCover = 0;   // %
    int code = 0;
    bool isDay = true;
    std::vector<WeatherSlot> quarterHours;  // next hour, 15-minute steps
    std::vector<WeatherSlot> hours;         // next 48 hours
    std::vector<WeatherDay> days;           // next 7 days
    std::vector<WeatherCity> cities;        // regional map
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
    void fetchRegional(WeatherSnapshot *w);

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
