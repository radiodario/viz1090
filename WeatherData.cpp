// WeatherData: local forecast from Open-Meteo. See WeatherData.h.

#include "WeatherData.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using json = nlohmann::json;

static const int REFRESH_S = 600;
static const int RETRY_S = 60;

static size_t appendBody(char *data, size_t size, size_t count, void *out) {
    static_cast<std::string *>(out)->append(data, size * count);
    return size * count;
}

// "2026-11-16T22:45" or "2026-11-16" in the forecast's local time
static time_t parseLocalTime(const std::string &s) {
    struct tm t = {};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if(sscanf(s.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) < 3) {
        return 0;
    }
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    return mktime(&t);
}

std::string weatherDescription(int code) {
    switch(code) {
        case 0: return "Clear";
        case 1: return "Mainly clear";
        case 2: return "Partly cloudy";
        case 3: return "Overcast";
        case 45: case 48: return "Fog";
        case 51: return "Light drizzle";
        case 53: return "Drizzle";
        case 55: return "Heavy drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Light showers";
        case 81: return "Showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunder and hail";
        default: return "Unknown";
    }
}

WeatherData::WeatherData() {
    lat = 0;
    lon = 0;
    running = false;
}

WeatherData::~WeatherData() {
    running = false;
    if(worker.joinable()) {
        worker.join();
    }
}

void WeatherData::start(double lat, double lon) {
    // ~1 km is plenty for a forecast, and sends less precise location data
    this->lat = std::round(lat * 100.0) / 100.0;
    this->lon = std::round(lon * 100.0) / 100.0;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    running = true;
    worker = std::thread(&WeatherData::run, this);
}

void WeatherData::run() {
    while(running) {
        int wait = fetch() ? REFRESH_S : RETRY_S;
        for(int i = 0; i < wait * 4 && running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

bool WeatherData::fetch() {
    char url[1024];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,relative_humidity_2m,is_day,dew_point_2m,pressure_msl,visibility,cloud_cover"
             "&minutely_15=precipitation,weather_code,temperature_2m,is_day&forecast_minutely_15=4"
             "&hourly=temperature_2m,precipitation_probability,precipitation,weather_code,is_day,wind_speed_10m,wind_direction_10m&forecast_hours=48"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,wind_speed_10m_max,wind_direction_10m_dominant&forecast_days=7"
             "&timezone=auto&wind_speed_unit=mph",
             lat, lon);

    std::string body;
    long code = 0;
    CURL *curl = curl_easy_init();
    if(!curl) {
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "viz1090-weather");
    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_easy_cleanup(curl);

    if(res != CURLE_OK || code != 200) {
        std::lock_guard<std::mutex> guard(lock);
        lastError = res != CURLE_OK ? curl_easy_strerror(res) : "HTTP " + std::to_string(code);
        return false;
    }

    WeatherSnapshot w;
    try {
        json doc = json::parse(body);

        const json &c = doc.at("current");
        w.temp = c.value("temperature_2m", 0.0f);
        w.feelsLike = c.value("apparent_temperature", 0.0f);
        w.windMph = c.value("wind_speed_10m", 0.0f);
        w.windDir = c.value("wind_direction_10m", 0);
        w.humidity = c.value("relative_humidity_2m", 0);
        w.dewPoint = c.value("dew_point_2m", 0.0f);
        w.pressure = c.value("pressure_msl", 0.0f);
        w.visibility = c.value("visibility", 0.0f);
        w.cloudCover = c.value("cloud_cover", 0);
        w.code = c.value("weather_code", 0);
        w.isDay = c.value("is_day", 1) != 0;

        auto num = [](const json &arr, size_t i, float fallback) {
            return (i < arr.size() && arr[i].is_number()) ? arr[i].get<float>() : fallback;
        };

        const json &q = doc.at("minutely_15");
        for(size_t i = 0; i < q.at("time").size(); i++) {
            WeatherSlot s;
            s.time = parseLocalTime(q["time"][i].get<std::string>());
            s.temp = num(q["temperature_2m"], i, w.temp);
            s.code = (int)num(q["weather_code"], i, 0);
            s.precip = num(q["precipitation"], i, 0);
            s.precipProb = -1;
            s.isDay = num(q["is_day"], i, 1) != 0;
            w.quarterHours.push_back(s);
        }

        const json &h = doc.at("hourly");
        for(size_t i = 0; i < h.at("time").size(); i++) {
            WeatherSlot s;
            s.time = parseLocalTime(h["time"][i].get<std::string>());
            s.temp = num(h["temperature_2m"], i, 0);
            s.code = (int)num(h["weather_code"], i, 0);
            s.precip = num(h["precipitation"], i, 0);
            s.precipProb = (int)num(h["precipitation_probability"], i, -1);
            s.isDay = num(h["is_day"], i, 1) != 0;
            s.windMph = num(h["wind_speed_10m"], i, 0);
            s.windDir = (int)num(h["wind_direction_10m"], i, 0);
            w.hours.push_back(s);
        }

        const json &d = doc.at("daily");
        for(size_t i = 0; i < d.at("time").size(); i++) {
            WeatherDay day;
            day.date = parseLocalTime(d["time"][i].get<std::string>());
            day.code = (int)num(d["weather_code"], i, 0);
            day.tempMax = num(d["temperature_2m_max"], i, 0);
            day.tempMin = num(d["temperature_2m_min"], i, 0);
            day.precipProb = (int)num(d["precipitation_probability_max"], i, -1);
            day.sunrise = d.contains("sunrise") && d["sunrise"][i].is_string() ? parseLocalTime(d["sunrise"][i].get<std::string>()) : 0;
            day.sunset = d.contains("sunset") && d["sunset"][i].is_string() ? parseLocalTime(d["sunset"][i].get<std::string>()) : 0;
            day.windMph = d.contains("wind_speed_10m_max") ? num(d["wind_speed_10m_max"], i, 0) : 0;
            day.windDir = d.contains("wind_direction_10m_dominant") ? (int)num(d["wind_direction_10m_dominant"], i, 0) : 0;
            w.days.push_back(day);
        }
    } catch(const std::exception &e) {
        std::lock_guard<std::mutex> guard(lock);
        lastError = e.what();
        return false;
    }

    fetchRegional(&w);

    w.valid = true;
    w.fetched = time(NULL);

    std::lock_guard<std::mutex> guard(lock);
    current = w;
    lastError = "";
    return true;
}

// Current temperature and conditions for a handful of cities, for the
// regional map, in one request (Open-Meteo takes lists of coordinates).
void WeatherData::fetchRegional(WeatherSnapshot *w) {
    struct City { const char *name; double lat, lon; };
    static const City CITIES[] = {
        {"London", 51.507, -0.128}, {"Birmingham", 52.486, -1.890}, {"Manchester", 53.481, -2.243},
        {"Bristol", 51.455, -2.588}, {"Cardiff", 51.481, -3.179}, {"Norwich", 52.630, 1.297},
        {"Brighton", 50.823, -0.137}, {"Southampton", 50.910, -1.404}, {"Cambridge", 52.205, 0.122},
        {"Oxford", 51.752, -1.258}, {"Dover", 51.128, 1.313}, {"Paris", 48.857, 2.352},
        {"Brussels", 50.850, 4.352}, {"Amsterdam", 52.370, 4.895}, {"Lille", 50.630, 3.057},
        {"Rouen", 49.443, 1.099}, {"Plymouth", 50.376, -4.143}, {"Leeds", 53.801, -1.549},
    };
    const int count = sizeof(CITIES) / sizeof(CITIES[0]);

    std::string lats, lons;
    for(int i = 0; i < count; i++) {
        char a[32], b[32];
        snprintf(a, sizeof(a), "%s%.3f", i ? "," : "", CITIES[i].lat);
        snprintf(b, sizeof(b), "%s%.3f", i ? "," : "", CITIES[i].lon);
        lats += a;
        lons += b;
    }
    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + lats + "&longitude=" + lons +
                      "&current=temperature_2m,weather_code,is_day&timezone=auto";

    std::string body;
    long code = 0;
    CURL *curl = curl_easy_init();
    if(!curl) {
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "viz1090-weather");
    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_easy_cleanup(curl);
    if(res != CURLE_OK || code != 200) {
        return;  // the regional map just stays empty
    }

    try {
        json doc = json::parse(body);
        if(!doc.is_array()) {
            return;
        }
        for(int i = 0; i < count && i < (int)doc.size(); i++) {
            const json &c = doc[i].at("current");
            WeatherCity city;
            city.name = CITIES[i].name;
            city.lat = CITIES[i].lat;
            city.lon = CITIES[i].lon;
            city.temp = c.value("temperature_2m", 0.0f);
            city.code = c.value("weather_code", 0);
            city.isDay = c.value("is_day", 1) != 0;
            w->cities.push_back(city);
        }
    } catch(const std::exception &) {
    }
}

WeatherSnapshot WeatherData::snapshot() {
    std::lock_guard<std::mutex> guard(lock);
    return current;
}

std::string WeatherData::status() {
    std::lock_guard<std::mutex> guard(lock);
    if(!current.valid) {
        return lastError.empty() ? "loading" : lastError;
    }
    long mins = (time(NULL) - current.fetched) / 60;
    return "updated " + std::to_string(mins) + "m ago";
}
