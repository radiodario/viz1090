// viz1090 weather page: a local forecast in rotating full-screen segments,
// like the old weather channels, with animated pixel-art glyphs. Data from
// WeatherData (Open-Meteo) and RadarData (RainViewer).

#include "View.h"
#include "Label.h"
#include "SDL2/SDL2_gfxPrimitives.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

//
// Pixel-art glyphs: 16x16 masks, layered. '.' is transparent.
//

static const char *GLYPH_SUN[] = {
    ".......YY.......",
    ".......YY.......",
    "..Y..........Y..",
    "...Y..YYYY..Y...",
    ".....YYYYYY.....",
    "....YYYYYYYY....",
    "....YYYYYYYY....",
    "YY..YYYYYYYY..YY",
    "YY..YYYYYYYY..YY",
    "....YYYYYYYY....",
    "....YYYYYYYY....",
    ".....YYYYYY.....",
    "...Y..YYYY..Y...",
    "..Y..........Y..",
    ".......YY.......",
    ".......YY.......",
};

static const char *GLYPH_MOON[] = {
    "......MMMMM.....",
    "....MMMMMM......",
    "...MMMMMM.......",
    "..MMMMMM........",
    "..MMMMM.........",
    ".MMMMMM.........",
    ".MMMMM..........",
    ".MMMMM..........",
    ".MMMMM..........",
    ".MMMMMM.........",
    "..MMMMMM......M.",
    "..MMMMMMM....MM.",
    "...MMMMMMMMMMM..",
    "....MMMMMMMMM...",
    "......MMMMM.....",
    "................",
};

static const char *GLYPH_CLOUD[] = {
    "................",
    "................",
    "................",
    "................",
    "......WWWW......",
    ".....WWWWWW.....",
    "...WWWWWWWWWW...",
    "..WWWWWWWWWWWW..",
    ".WWWWWWWWWWWWWW.",
    "WWWWWWWWWWWWWWWW",
    "WWWWWWWWWWWWWWWW",
    "GWWWWWWWWWWWWWWG",
    ".GGGGGGGGGGGGGG.",
    "................",
    "................",
    "................",
};

static const char *GLYPH_DROPS[] = {
    "..B....B....B...",
    ".B....B....B....",
    "................",
    "....B....B....B.",
    "...B....B....B..",
};

static const char *GLYPH_DRIZZLE[] = {
    "..B......B......",
    "................",
    "......B......B..",
};

static const char *GLYPH_SNOW[] = {
    "..C.....C.....C.",
    ".CCC...CCC...CCC",
    "..C.....C.....C.",
    "................",
    ".....C.....C....",
    "....CCC...CCC...",
    ".....C.....C....",
};

static const char *GLYPH_SLEET[] = {
    "..B.....C.....B.",
    ".B.....CCC...B..",
    "........C.......",
    ".....C.....B....",
    "....CCC...B.....",
    ".....C..........",
};

static const char *GLYPH_BOLT[] = {
    "........LL......",
    ".......LL.......",
    "......LLLLL.....",
    "........LL......",
    ".......LL.......",
    "......L.........",
};

static const char *GLYPH_FOG[] = {
    "................",
    "................",
    "................",
    "GGGGGGGGGGGG....",
    "................",
    "..GGGGGGGGGGGGGG",
    "................",
    "GGGGGGGGGGGGGG..",
    "................",
    "...GGGGGGGGGGGG.",
    "................",
    "GGGGGGGGGGGG....",
};

struct GlyphPalette {
    bool darkCloud;
};

static SDL_Color glyphColor(char c, bool darkCloud) {
    switch(c) {
        case 'Y': return {255, 196, 40, 255};
        case 'M': return {236, 232, 190, 255};
        case 'W': return darkCloud ? SDL_Color{128, 138, 156, 255} : SDL_Color{236, 242, 252, 255};
        case 'G': return darkCloud ? SDL_Color{84, 92, 108, 255} : SDL_Color{150, 162, 180, 255};
        case 'B': return {90, 170, 255, 255};
        case 'C': return {226, 244, 255, 255};
        case 'L': return {255, 232, 60, 255};
        default: return {255, 0, 255, 255};
    }
}

// Draw one mask; (x, y) is the glyph's top-left, px the pixel block size.
// rayPhase >= 0 makes the sun's rays (pixels away from the disc) twinkle.
static void drawMask(SDL_Renderer *r, const char **mask, int rows, int x, int y, int px, bool darkCloud, int rayPhase = -1) {
    for(int row = 0; row < rows; row++) {
        const char *line = mask[row];
        for(int col = 0; line[col]; col++) {
            if(line[col] == '.') {
                continue;
            }
            if(rayPhase >= 0 && line[col] == 'Y') {
                float dx = col - 7.5f, dy = row - 7.5f;
                if(dx * dx + dy * dy > 4.6f * 4.6f && ((row + col + rayPhase) % 3) == 0) {
                    continue;
                }
            }
            SDL_Color c = glyphColor(line[col], darkCloud);
            boxRGBA(r, x + col * px, y + row * px, x + (col + 1) * px - 1, y + (row + 1) * px - 1, c.r, c.g, c.b, 255);
        }
    }
}

#define ROWS(m) ((int)(sizeof(m) / sizeof((m)[0])))

// one layer of a glyph, positioned in glyph cells
struct GlyphLayer {
    const char **mask;
    int rows;
    int dx, dy;
    bool dark;
    int rayPhase;
};

// visible cells of a layer, for centring
static void layerBounds(const GlyphLayer &l, int *x0, int *y0, int *x1, int *y1) {
    for(int row = 0; row < l.rows; row++) {
        for(int col = 0; l.mask[row][col]; col++) {
            if(l.mask[row][col] != '.') {
                *x0 = std::min(*x0, l.dx + col);
                *x1 = std::max(*x1, l.dx + col + 1);
                *y0 = std::min(*y0, l.dy + row);
                *y1 = std::max(*y1, l.dy + row + 1);
            }
        }
    }
}

// cheap per-particle pseudo-random in [0, 1)
static float particleRand(int k, int salt) {
    unsigned int h = (unsigned int)(k * 2654435761u) ^ (unsigned int)(salt * 40503u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xffff) / 65536.0f;
}

// A WMO weather code as a pixel glyph centred on (cx, cy), px = cell size.
// anim >= 0 (seconds) animates it: sun rays twinkle, clouds drift, each rain
// drop and snowflake falls on its own, lightning flickers.
void View::drawWeatherGlyph(int code, bool isDay, int cx, int cy, int px, float anim) {
    bool animated = anim >= 0;

    bool clear = code == 0;
    bool partly = code == 1 || code == 2;
    bool overcast = code == 3;
    bool fog = code == 45 || code == 48;
    bool drizzle = code >= 51 && code <= 55;
    bool sleet = code == 56 || code == 57 || code == 66 || code == 67;
    bool rain = code == 61 || code == 63;
    bool heavyRain = code == 65 || code == 82;
    bool showers = code == 80 || code == 81;
    bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    bool thunder = code >= 95;
    bool precip = drizzle || sleet || rain || heavyRain || showers || snow || thunder;

    const char **sky = isDay ? GLYPH_SUN : GLYPH_MOON;
    int skyRows = isDay ? ROWS(GLYPH_SUN) : ROWS(GLYPH_MOON);
    int rayPhase = (animated && isDay) ? (int)(anim * 4.0f) % 3 : -1;

    // build the layers in cell coordinates
    std::vector<GlyphLayer> layers;
    int cloudDx = 0, cloudDy = 0;
    if(clear) {
        layers.push_back({sky, skyRows, 0, 0, false, rayPhase});
    } else if(fog) {
        layers.push_back({GLYPH_FOG, ROWS(GLYPH_FOG), 0, 2, false, -1});
    } else {
        if(partly || showers) {
            layers.push_back({sky, skyRows, -3, -4, false, rayPhase});
            cloudDx = 2;
            cloudDy = partly ? 2 : 0;
        }
        bool dark = overcast || heavyRain || thunder;
        layers.push_back({GLYPH_CLOUD, ROWS(GLYPH_CLOUD), cloudDx, cloudDy, dark, -1});
    }

    // bounds: the layers, plus room under the cloud for precipitation
    int bx0 = 1000, by0 = 1000, bx1 = -1000, by1 = -1000;
    for(const GlyphLayer &l : layers) {
        layerBounds(l, &bx0, &by0, &bx1, &by1);
    }
    const int PRECIP_ROWS = 6;
    int cloudBottom = cloudDy + 13;  // first row under the cloud
    if(precip) {
        by1 = std::max(by1, cloudBottom + PRECIP_ROWS);
    }

    // centre the visible shape on (cx, cy)
    int ox = cx - (bx0 + bx1) * px / 2;
    int oy = cy - (by0 + by1) * px / 2;
    int drift = animated ? (int)lroundf(sinf(anim * 0.8f)) * px : 0;

    // precipitation first, so it tucks under the cloud
    if(precip && !thunder) {
        int count = drizzle ? 4 : snow ? 6 : heavyRain ? 12 : 8;
        float speed = drizzle ? 0.55f : snow ? 0.35f : heavyRain ? 1.4f : 1.0f;  // falls per second
        int length = (drizzle || snow) ? 1 : 2;
        for(int k = 0; k < count; k++) {
            // evenly spaced columns with a little jitter, each drop on its own timing
            int col = cloudDx + 1 + (k * 14) / count + (int)(particleRand(k, 1) * 1.5f);
            float phase = particleRand(k, 2);
            float rate = speed * (0.75f + 0.5f * particleRand(k, 3));
            float f = animated ? fmodf(anim * rate + phase, 1.0f) : phase;
            int row = cloudBottom - 1 + (int)(f * PRECIP_ROWS);
            int sway = (snow && animated) ? (int)lroundf(sinf(anim * 2.0f + k)) : 0;
            char kind = snow ? 'C' : (sleet && (k % 2)) ? 'C' : 'B';
            SDL_Color c = glyphColor(kind, false);
            int x = ox + (col + sway) * px + drift;
            int y = oy + row * px;
            boxRGBA(renderer, x, y, x + px - 1, y + length * px - 1, c.r, c.g, c.b, 255);
        }
    }
    if(thunder) {
        // the bolt flashes: a double flicker every couple of seconds
        float t = animated ? fmodf(anim, 2.4f) : 0.0f;
        bool on = !animated || t < 0.12f || (t > 0.25f && t < 0.4f);
        if(on) {
            drawMask(renderer, GLYPH_BOLT, ROWS(GLYPH_BOLT), ox + cloudDx * px + drift, oy + cloudBottom * px, px, false);
        }
    }

    for(const GlyphLayer &l : layers) {
        bool isCloud = l.mask == GLYPH_CLOUD || l.mask == GLYPH_FOG;
        drawMask(renderer, l.mask, l.rows, ox + l.dx * px + (isCloud ? drift : 0), oy + l.dy * px, px, l.dark, l.rayPhase);
    }
}

//
// Moon
//

static const double SYNODIC_MONTH = 29.530588853;
static const time_t KNOWN_NEW_MOON = 947182440;  // 2000-01-06 18:14 UTC

static double moonPhase(time_t t) {  // 0 = new, 0.5 = full
    double p = fmod((t - KNOWN_NEW_MOON) / 86400.0 / SYNODIC_MONTH, 1.0);
    return p < 0 ? p + 1.0 : p;
}

static std::string moonPhaseName(double p) {
    if(p < 0.03 || p > 0.97) return "New moon";
    if(p < 0.22) return "Waxing crescent";
    if(p < 0.28) return "First quarter";
    if(p < 0.47) return "Waxing gibbous";
    if(p < 0.53) return "Full moon";
    if(p < 0.72) return "Waning gibbous";
    if(p < 0.78) return "Last quarter";
    return "Waning crescent";
}

// next time the phase reaches `target` (0 new, 0.5 full) after t
static time_t nextPhase(time_t t, double target) {
    double p = moonPhase(t);
    double days = fmod(target - p + 1.0, 1.0) * SYNODIC_MONTH;
    return t + (time_t)(days * 86400.0);
}

// The moon as a pixel disc with the lit part for its phase.
static void drawMoonDisc(SDL_Renderer *r, int cx, int cy, int cells, int px, double phase) {
    double k = cos(2.0 * M_PI * phase);  // 1 at new moon, -1 at full
    for(int j = 0; j < cells; j++) {
        for(int i = 0; i < cells; i++) {
            double x = (i + 0.5) / cells * 2.0 - 1.0;
            double y = (j + 0.5) / cells * 2.0 - 1.0;
            if(x * x + y * y > 1.0) {
                continue;
            }
            double edge = sqrt(1.0 - y * y);
            bool lit = phase < 0.5 ? x >= k * edge : x <= -k * edge;
            SDL_Color c = lit ? SDL_Color{236, 232, 190, 255} : SDL_Color{40, 40, 48, 255};
            int x0 = cx - cells * px / 2 + i * px;
            int y0 = cy - cells * px / 2 + j * px;
            boxRGBA(r, x0, y0, x0 + px - 1, y0 + px - 1, c.r, c.g, c.b, 255);
        }
    }
}

//
// Layout helpers
//

// same palette as the map and departures pages: black, greys, white,
// teal for data, orange for the headline
static const SDL_Color PANEL_EDGE = {64, 64, 64, 255};
static const SDL_Color ORANGE = {253, 151, 31, 255};
static const SDL_Color GREY = {127, 127, 127, 255};
static const SDL_Color DARK = {48, 48, 52, 255};
static const SDL_Color WHITE = {255, 255, 255, 255};
static const SDL_Color TEAL = {0, 255, 174, 255};

static std::string compass(int degrees) {
    const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[((degrees + 22) / 45) % 8];
}

static std::string tempString(float t) {
    return std::to_string((int)lroundf(t));
}

static std::string clockString(time_t t) {
    char s[16];
    strftime(s, sizeof(s), "%H:%M", localtime(&t));
    return s;
}

// draw text with a small drop shadow (broadcast style, and easier to read
// over the radar); return its width
static int putText(SDL_Renderer *r, TTF_Font *font, const std::string &s, int x, int y, SDL_Color color) {
    if(!font || s.empty()) {
        return 0;
    }
    int off = std::max(1, TTF_FontHeight(font) / 14);
    Label label;
    label.setFont(font);
    label.setColor({0, 0, 0, 255});
    label.setPosition(x + off, y + off);
    label.setText(s);
    label.draw(r);
    label.setColor(color);
    label.setPosition(x, y);
    label.setText(s);  // the colour only applies when the text is rendered
    label.draw(r);
    int w = 0, h = 0;
    TTF_SizeUTF8(font, s.c_str(), &w, &h);
    return w;
}

static int textWidth(TTF_Font *font, const std::string &s) {
    int w = 0, h = 0;
    if(font) {
        TTF_SizeUTF8(font, s.c_str(), &w, &h);
    }
    return w;
}

static int fontHeight(TTF_Font *font) {
    return font ? TTF_FontHeight(font) : 0;
}

static void putCentred(SDL_Renderer *r, TTF_Font *font, const std::string &s, int cx, int y, SDL_Color color) {
    putText(r, font, s, cx - textWidth(font, s) / 2, y, color);
}

// temperature with a drawn degree ring (the pixel fonts have no degree sign)
static int putTemp(SDL_Renderer *r, TTF_Font *font, float t, int x, int y, SDL_Color color) {
    std::string s = tempString(t);
    int w = putText(r, font, s, x, y, color);
    int h = fontHeight(font);
    int rad = std::max(2, h / 9);
    int ring = std::max(1, rad / 2);
    for(int i = 0; i < ring; i++) {
        circleRGBA(r, x + w + rad + 1, y + h / 5 + rad, rad - i, color.r, color.g, color.b, 255);
    }
    return w + 2 * rad + 3;
}

static int tempWidth(TTF_Font *font, float t) {
    int h = fontHeight(font);
    return textWidth(font, tempString(t)) + 2 * std::max(2, h / 9) + 3;
}

static void putTempCentred(SDL_Renderer *r, TTF_Font *font, float t, int cx, int y, SDL_Color color) {
    putTemp(r, font, t, cx - tempWidth(font, t) / 2, y, color);
}

//
// Segments
//

enum WeatherSegment { SEG_NOW, SEG_TEXT, SEG_NEXT_HOUR, SEG_12H, SEG_48H, SEG_WEEK, SEG_REGIONAL, SEG_RADAR, SEG_SUN_MOON, SEG_ALMANAC, SEG_COUNT };
static const char *SEGMENT_NAMES[SEG_COUNT] = {"Current Conditions", "Local Forecast", "Next Hour", "Next 12 Hours", "48 Hours",
                                                "Extended Forecast", "Regional Conditions", "Radar", "Sun and Moon", "Almanac"};
static const int SEGMENT_SECONDS[SEG_COUNT] = {12, 15, 10, 10, 10, 10, 12, 16, 10, 10};

//
// Text forecast, written from the hourly data
//

struct ForecastPeriod {
    std::string name;
    time_t start, end;
    bool night;
};

// A forecast period boiled down to what the page shows
struct ForecastSummary {
    std::string name;
    bool night;
    int code;          // most notable weather in the period
    float temp;        // high by day, low at night
    int rainProb;      // %, max over the period
    float rainMm;
    int windLo, windHi;
    std::string windDir;
    std::string trend; // e.g. "Drying out later", or empty
};

static bool summarise(const WeatherSnapshot &w, const ForecastPeriod &p, ForecastSummary *out) {
    std::vector<const WeatherSlot *> slots;
    for(const WeatherSlot &s : w.hours) {
        if(s.time >= p.start && s.time < p.end) {
            slots.push_back(&s);
        }
    }
    if(slots.empty()) {
        return false;
    }
    ForecastSummary f;
    f.name = p.name;
    f.night = p.night;
    f.code = 0;
    f.rainProb = 0;
    f.rainMm = 0;
    float tMin = 1e9f, tMax = -1e9f, wMin = 1e9f, wMax = 0;
    int windDeg = 0;
    for(const WeatherSlot *s : slots) {
        f.code = std::max(f.code, s->code);  // higher WMO codes are the more notable weather
        f.rainProb = std::max(f.rainProb, s->precipProb);
        f.rainMm += s->precip;
        tMin = std::min(tMin, s->temp);
        tMax = std::max(tMax, s->temp);
        wMin = std::min(wMin, s->windMph);
        if(s->windMph >= wMax) {
            wMax = s->windMph;
            windDeg = s->windDir;
        }
    }
    f.temp = p.night ? tMin : tMax;
    auto round5 = [](float v) { return std::max(5, (int)lroundf(v / 5.0f) * 5); };
    f.windLo = round5(wMin);
    f.windHi = round5(wMax);
    f.windDir = compass(windDeg);

    // does the rain come or go during the period?
    size_t half = slots.size() / 2;
    int early = 0, late = 0;
    for(size_t i = 0; i < slots.size(); i++) {
        (i < half ? early : late) = std::max(i < half ? early : late, slots[i]->precipProb);
    }
    if(slots.size() >= 4 && early >= 50 && late < 25) {
        f.trend = "Drying out later";
    } else if(slots.size() >= 4 && early < 25 && late >= 50) {
        f.trend = "Rain arriving later";
    }
    *out = f;
    return true;
}

static std::vector<ForecastSummary> forecastPeriods(const WeatherSnapshot &w, time_t now) {
    struct tm lt = *localtime(&now);
    auto at = [&](int dayOffset, int hour) {
        struct tm t = lt;
        t.tm_mday += dayOffset;
        t.tm_hour = hour;
        t.tm_min = 0;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        return mktime(&t);
    };
    std::vector<ForecastPeriod> periods;
    if(lt.tm_hour >= 5 && lt.tm_hour < 17) {
        periods.push_back({"Today", now, at(0, 18), false});
        periods.push_back({"Tonight", at(0, 18), at(1, 6), true});
        periods.push_back({"Tomorrow", at(1, 6), at(1, 18), false});
    } else {
        int base = lt.tm_hour >= 17 ? 1 : 0;  // after midnight, "tomorrow" is today
        periods.push_back({"Tonight", now, at(base, 6), true});
        periods.push_back({base ? "Tomorrow" : "Today", at(base, 6), at(base, 18), false});
        periods.push_back({base ? "Tomorrow night" : "Tonight", at(base, 18), at(base + 1, 6), true});
    }
    std::vector<ForecastSummary> out;
    for(const ForecastPeriod &p : periods) {
        ForecastSummary f;
        if(summarise(w, p, &f)) {
            out.push_back(f);
        }
    }
    return out;
}

static std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

float View::weatherCycleSeconds() {
    float total = 0;
    for(int i = 0; i < SEG_COUNT; i++) {
        total += SEGMENT_SECONDS[i];
    }
    return total;
}

void View::nextWeatherSegment(int dir) {
    weatherSegment = ((weatherSegment + dir) % SEG_COUNT + SEG_COUNT) % SEG_COUNT;
    weatherSegmentShown = std::chrono::steady_clock::now();
}

struct Area {
    int x, y, w, h;
};

void View::drawWeather() {
    int W = screen_width;
    int H = screen_height;
    int m = std::max(4, H / 50);
    float u = H / 36.0f;

    auto now = std::chrono::steady_clock::now();
    float anim = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000000 / 1000.0f;

    // advance the rotation
    float segElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - weatherSegmentShown).count() / 1000.0f;
    if(segElapsed >= SEGMENT_SECONDS[weatherSegment]) {
        nextWeatherSegment(1);
        segElapsed = 0;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    WeatherSnapshot w = weather->snapshot();
    time_t nowWall = time(NULL);

    // header: clock, then this segment's title; date on the right
    TTF_Font *clockFont = transitClockFont ? transitClockFont : weatherHeadFont;
    // same font, size and colour as the departures board's "Departures"
    TTF_Font *titleFont = transitHeadFont ? transitHeadFont : weatherHeadFont;
    int headH = std::max(fontHeight(titleFont), fontHeight(clockFont));
    int tickerH = fontHeight(weatherBodyFont) + m;
    int progressH = std::max(2, H / 300);
    Area area = {m, m + headH + m, W - 2 * m, H - (m + headH + m) - m - tickerH - progressH};

    if(weatherSegment == SEG_RADAR) {
        drawWeatherRadar(area.y, area.y + area.h + m);
    } else if(weatherSegment == SEG_REGIONAL && w.valid) {
        drawWeatherRegional(w, area.y, area.y + area.h + m, anim);
    }

    int hx = m;
    hx += putText(renderer, clockFont, clockString(nowWall), hx, m, WHITE) + 2 * m;
    putText(renderer, titleFont, SEGMENT_NAMES[weatherSegment], hx, m + headH - fontHeight(titleFont), ORANGE);
    if(weatherSegment == SEG_RADAR) {
        drawRadarLegend(W - m, m, headH);
    } else {
        char date[32];
        strftime(date, sizeof(date), "%a %d %b", localtime(&nowWall));
        putText(renderer, weatherBodyFont, date, W - m - textWidth(weatherBodyFont, date), m + headH - fontHeight(weatherBodyFont), GREY);
    }
    lineRGBA(renderer, 0, m + headH + m / 2, W, m + headH + m / 2, PANEL_EDGE.r, PANEL_EDGE.g, PANEL_EDGE.b, 255);

    // ticker along the bottom: short lines about now, a few seconds each
    int tickerY = H - progressH - tickerH;
    boxRGBA(renderer, 0, tickerY, W, H, 12, 12, 14, 255);
    lineRGBA(renderer, 0, tickerY, W, tickerY, PANEL_EDGE.r, PANEL_EDGE.g, PANEL_EDGE.b, 255);
    if(w.valid) {
        std::vector<std::string> lines;
        lines.push_back("Now " + weatherDescription(w.code) + ", " + tempString(w.temp) + "\xC2\xB0" "C");
        lines.push_back("Wind " + compass(w.windDir) + " " + std::to_string((int)lroundf(w.windMph)) + " mph");
        lines.push_back("Humidity " + std::to_string(w.humidity) + "%");
        if(!w.days.empty()) {
            lines.push_back("Sunrise " + clockString(w.days[0].sunrise) + ", sunset " + clockString(w.days[0].sunset));
            if(w.days[0].precipProb >= 0) {
                lines.push_back("Chance of rain today " + std::to_string(w.days[0].precipProb) + "%");
            }
        }
        if(w.days.size() > 1) {
            lines.push_back("Tomorrow " + weatherDescription(w.days[1].code) + ", high " + tempString(w.days[1].tempMax) + "\xC2\xB0" "C");
        }
        lines.push_back("Radar: RainViewer   Forecast: Open-Meteo");
        std::string text;
        for(const std::string &l : lines) {
            text += (text.empty() ? "" : "   \xE2\x80\xA2   ") + upper(l);
        }
        drawTicker(text, tickerY + m / 2, anim);
    }

    // progress through this segment
    float progress = std::min(1.0f, segElapsed / SEGMENT_SECONDS[weatherSegment]);
    boxRGBA(renderer, 0, H - progressH, (int)(W * progress), H, TEAL.r, TEAL.g, TEAL.b, 255);

    if(!w.valid) {
        putText(renderer, weatherTextFont, "Fetching forecast... " + weather->status(), area.x, area.y, GREY);
        return;
    }

    switch(weatherSegment) {
    case SEG_NOW: {
        // left: temperature, conditions, glyph, wind
        int leftW = area.w * 9 / 20;
        int lcx = area.x + leftW / 2;
        int y = area.y;
        putTempCentred(renderer, weatherBigFont, w.temp, lcx, y, WHITE);
        y += fontHeight(weatherBigFont);
        putCentred(renderer, weatherHeadFont, weatherDescription(w.code), lcx, y, ORANGE);
        y += fontHeight(weatherHeadFont);
        int windY = area.y + area.h - fontHeight(weatherBodyFont);
        int glyphSpace = windY - y - m;
        int glyphPx = std::max(2, std::min(glyphSpace / 20, leftW / 20));
        drawWeatherGlyph(w.code, w.isDay, lcx, y + glyphSpace / 2, glyphPx, anim);
        putCentred(renderer, weatherBodyFont, "Wind " + compass(w.windDir) + " " + std::to_string((int)lroundf(w.windMph)) + " mph", lcx, windY, WHITE);

        // right: labelled readings, values right-aligned
        int rx = area.x + leftW + 2 * m;
        int rRight = area.x + area.w;
        struct Row { std::string label; std::string value; bool temp; float t; };
        char pressure[16], visibility[16];
        snprintf(pressure, sizeof(pressure), "%d hPa", (int)lroundf(w.pressure));
        if(w.visibility >= 10000) {
            snprintf(visibility, sizeof(visibility), "%d km", (int)(w.visibility / 1000));
        } else {
            snprintf(visibility, sizeof(visibility), "%.1f km", w.visibility / 1000.0f);
        }
        std::vector<Row> rows = {
            {"Humidity", std::to_string(w.humidity) + "%", false, 0},
            {"Feels like", "", true, w.feelsLike},
            {"Dew point", "", true, w.dewPoint},
            {"Pressure", pressure, false, 0},
            {"Visibility", visibility, false, 0},
            {"Cloud", std::to_string(w.cloudCover) + "%", false, 0},
        };
        if(!w.days.empty() && w.days[0].precipProb >= 0) {
            rows.push_back({"Rain today", std::to_string(w.days[0].precipProb) + "%", false, 0});
        }
        int rowH = std::min(fontHeight(weatherHeadFont) * 3 / 2, area.h / (int)rows.size());
        int ry = area.y + std::max(0, (area.h - rowH * (int)rows.size()) / 2);
        for(const Row &row : rows) {
            putText(renderer, weatherBodyFont, row.label, rx, ry, GREY);
            if(row.temp) {
                putTemp(renderer, weatherBodyFont, row.t, rRight - tempWidth(weatherBodyFont, row.t), ry, WHITE);
            } else {
                putText(renderer, weatherBodyFont, row.value, rRight - textWidth(weatherBodyFont, row.value), ry, WHITE);
            }
            ry += rowH;
        }
        break;
    }

    case SEG_TEXT: {
        // the next three periods side by side: what, how warm, rain, wind
        std::vector<ForecastSummary> periods = forecastPeriods(w, nowWall);
        if(periods.empty()) {
            break;
        }
        int cols = (int)periods.size();
        int colW = area.w / cols;
        int textBlock = fontHeight(weatherHeadFont) + fontHeight(weatherBodyFont) + fontHeight(weatherBigFont) +
                        fontHeight(weatherHeadFont) + m + 2 * fontHeight(weatherBodyFont);
        int glyphPx = std::max(1, std::min(colW / 24, (area.h - textBlock - 3 * m) / 22));
        int blockH = textBlock + 22 * glyphPx + 2 * m;
        int top = area.y + std::max(0, (area.h - blockH) / 2);
        for(int i = 0; i < cols; i++) {
            const ForecastSummary &f = periods[i];
            int cx = area.x + colW * i + colW / 2;
            int y = top;
            putCentred(renderer, weatherHeadFont, f.name, cx, y, WHITE);
            y += fontHeight(weatherHeadFont) + m;
            drawWeatherGlyph(f.code, !f.night, cx, y + 11 * glyphPx, glyphPx, anim);
            y += 22 * glyphPx + m;
            putCentred(renderer, weatherBodyFont, weatherDescription(f.code), cx, y, ORANGE);
            y += fontHeight(weatherBodyFont);
            int labelW = textWidth(weatherHeadFont, f.night ? "Low " : "High ");
            int tw = labelW + tempWidth(weatherBigFont, f.temp);
            int tx = cx - tw / 2;
            putText(renderer, weatherHeadFont, f.night ? "Low" : "High", tx, y + fontHeight(weatherBigFont) - fontHeight(weatherHeadFont) - m / 2, GREY);
            putTemp(renderer, weatherBigFont, f.temp, tx + labelW, y, WHITE);
            y += fontHeight(weatherBigFont);
            std::string rain = f.rainProb >= 20 ? "Rain " + std::to_string(f.rainProb) + "%" : "Dry";
            if(f.rainProb >= 20 && f.rainMm >= 0.5f) {
                rain += ", " + std::to_string((int)lroundf(f.rainMm)) + " mm";
            }
            putCentred(renderer, weatherBodyFont, rain, cx, y, TEAL);
            y += fontHeight(weatherBodyFont);
            std::string wind = "Wind " + f.windDir + " " + std::to_string(f.windLo) +
                               (f.windHi != f.windLo ? "-" + std::to_string(f.windHi) : "") + " mph";
            putCentred(renderer, weatherBodyFont, wind, cx, y, GREY);
            y += fontHeight(weatherBodyFont);
            if(!f.trend.empty()) {
                putCentred(renderer, weatherTextFont, f.trend, cx, y + m / 2, WHITE);
            }
        }
        break;
    }

    case SEG_REGIONAL:
        // drawn under the header above
        break;

    case SEG_ALMANAC: {
        // sunrise and sunset, today and tomorrow
        int labelX = area.x;
        int col1 = area.x + area.w * 45 / 100;
        int col2 = area.x + area.w * 80 / 100;
        int y = area.y;
        int rowH = fontHeight(weatherBodyFont) + m / 2;
        if(w.days.size() > 1) {
            char d0[16], d1[16];
            strftime(d0, sizeof(d0), "%A", localtime(&w.days[0].date));
            strftime(d1, sizeof(d1), "%A", localtime(&w.days[1].date));
            putCentred(renderer, weatherBodyFont, d0, col1, y, ORANGE);
            putCentred(renderer, weatherBodyFont, d1, col2, y, ORANGE);
            y += rowH;
            putText(renderer, weatherBodyFont, "Sunrise", labelX, y, GREY);
            putCentred(renderer, weatherBodyFont, clockString(w.days[0].sunrise), col1, y, WHITE);
            putCentred(renderer, weatherBodyFont, clockString(w.days[1].sunrise), col2, y, WHITE);
            y += rowH;
            putText(renderer, weatherBodyFont, "Sunset", labelX, y, GREY);
            putCentred(renderer, weatherBodyFont, clockString(w.days[0].sunset), col1, y, WHITE);
            putCentred(renderer, weatherBodyFont, clockString(w.days[1].sunset), col2, y, WHITE);
            y += rowH + m;
        }
        lineRGBA(renderer, area.x, y, area.x + area.w, y, PANEL_EDGE.r, PANEL_EDGE.g, PANEL_EDGE.b, 255);
        y += m;

        // the next four moon phases
        putText(renderer, weatherBodyFont, "Moon phases", labelX, y, ORANGE);
        y += rowH;
        struct Phase { const char *name; double at; time_t when; };
        std::vector<Phase> phases = {{"New", 0.0, 0}, {"First", 0.25, 0}, {"Full", 0.5, 0}, {"Last", 0.75, 0}};
        for(Phase &ph : phases) {
            ph.when = nextPhase(nowWall, ph.at);
        }
        std::sort(phases.begin(), phases.end(), [](const Phase &a, const Phase &b) { return a.when < b.when; });
        int colW = area.w / 4;
        int discArea = area.y + area.h - y - 2 * rowH;
        int cells = 16;
        int px = std::max(1, std::min(colW * 85 / 100, discArea) / cells);
        for(int i = 0; i < 4; i++) {
            int cx = area.x + colW * i + colW / 2;
            putCentred(renderer, weatherBodyFont, phases[i].name, cx, y, WHITE);
            drawMoonDisc(renderer, cx, y + rowH + cells * px / 2, cells, px, phases[i].at == 0.0 ? 0.0001 : phases[i].at);
            char date[16];
            strftime(date, sizeof(date), "%d %b", localtime(&phases[i].when));
            putCentred(renderer, weatherBodyFont, date, cx, y + rowH + cells * px + m / 2, WHITE);
        }
        break;
    }

    case SEG_NEXT_HOUR: {
        bool wetNow = !w.quarterHours.empty() && w.quarterHours[0].precip > 0.05f;
        std::string summary = wetNow ? weatherDescription(w.quarterHours[0].code) + " for the next hour" : "Dry for the next hour";
        for(size_t i = 1; i < w.quarterHours.size(); i++) {
            bool wet = w.quarterHours[i].precip > 0.05f;
            if(wet != wetNow) {
                summary = (wet ? "Rain from " : "Drying up by ") + clockString(w.quarterHours[i].time);
                break;
            }
        }
        putText(renderer, weatherHeadFont, summary, area.x, area.y, WHITE);

        int cols = std::max(1, (int)w.quarterHours.size());
        int colW = area.w / cols;
        int glyphTop = area.y + fontHeight(weatherHeadFont) + m;
        int glyphPx = std::max(1, std::min(colW / 18, area.h / 45));
        int glyphH = 20 * glyphPx;
        int barTop = glyphTop + glyphH + fontHeight(weatherTextFont) + m / 2;
        int base = area.y + area.h - fontHeight(weatherTextFont) - m / 2;
        int maxBar = std::max(1, base - barTop);
        for(int i = 0; i < (int)w.quarterHours.size(); i++) {
            const WeatherSlot &s = w.quarterHours[i];
            int cx = area.x + colW * i + colW / 2;
            drawWeatherGlyph(s.code, s.isDay, cx, glyphTop + glyphH / 2, glyphPx, anim);
            int bar = (int)(std::min(1.0f, s.precip / 2.0f) * maxBar);  // 2 mm per 15 min = full
            int bw = colW / 3;
            boxRGBA(renderer, cx - bw / 2, base - maxBar, cx + bw / 2, base, 24, 24, 28, 255);
            if(bar > 0) {
                boxRGBA(renderer, cx - bw / 2, base - bar, cx + bw / 2, base, TEAL.r, TEAL.g, TEAL.b, 255);
            }
            char mm[16];
            snprintf(mm, sizeof(mm), "%.1fmm", s.precip);
            putCentred(renderer, weatherTextFont, mm, cx, base - maxBar - fontHeight(weatherTextFont), WHITE);
            putCentred(renderer, weatherTextFont, clockString(s.time), cx, base + m / 4, GREY);
        }
        break;
    }

    case SEG_12H: {
        size_t start = 0;
        while(start < w.hours.size() && w.hours[start].time + 3600 <= nowWall) {
            start++;
        }
        // two rows of six
        int cols = 6;
        int colW = area.w / cols;
        int rowH = area.h / 2;
        int labels = fontHeight(weatherTextFont) * 3 + fontHeight(weatherHeadFont);
        int glyphPx = std::max(1, std::min(colW / 18, (rowH - labels) / 20));
        int cellH = labels + 19 * glyphPx;
        for(int i = 0; i < 12 && start + i < w.hours.size(); i++) {
            const WeatherSlot &s = w.hours[start + i];
            int cx = area.x + colW * (i % cols) + colW / 2;
            int ry = area.y + rowH * (i / cols) + std::max(0, (rowH - cellH) / 2);
            putCentred(renderer, weatherTextFont, i == 0 ? "Now" : clockString(s.time), cx, ry, GREY);
            drawWeatherGlyph(s.code, s.isDay, cx, ry + fontHeight(weatherTextFont) + 9 * glyphPx, glyphPx, anim);
            int ty = ry + fontHeight(weatherTextFont) + 19 * glyphPx;
            putTempCentred(renderer, weatherHeadFont, s.temp, cx, ty, WHITE);
            if(s.precipProb >= 0) {
                putCentred(renderer, weatherTextFont, std::to_string(s.precipProb) + "%", cx, ty + fontHeight(weatherHeadFont), TEAL);
            }
            putCentred(renderer, weatherTextFont, compass(s.windDir) + " " + std::to_string((int)lroundf(s.windMph)) + " mph",
                       cx, ty + fontHeight(weatherHeadFont) + fontHeight(weatherTextFont), GREY);
        }
        break;
    }

    case SEG_48H: {
        // temperature line over chance-of-rain bars, nights shaded,
        // with clock times and day names along the bottom
        if(w.hours.empty()) {
            break;
        }
        int n = (int)w.hours.size();
        int bodyH = fontHeight(weatherBodyFont);
        int textH = fontHeight(weatherTextFont);

        // legend
        int lx = area.x;
        int ly = area.y;
        int sw = bodyH * 3 / 2;
        thickLineRGBA(renderer, lx, ly + bodyH / 2, lx + sw, ly + bodyH / 2, std::max(2, H / 300), 255, 255, 255, 255);
        lx += sw + m / 2;
        lx += putText(renderer, weatherBodyFont, "Temperature", lx, ly, WHITE) + 2 * m;
        boxRGBA(renderer, lx, ly + bodyH / 4, lx + sw, ly + bodyH * 3 / 4, 24, 110, 96, 255);
        lx += sw + m / 2;
        lx += putText(renderer, weatherBodyFont, "Chance of rain", lx, ly, TEAL) + 2 * m;
        boxRGBA(renderer, lx, ly + bodyH / 4, lx + sw, ly + bodyH * 3 / 4, 30, 30, 38, 255);
        lx += sw + m / 2;
        putText(renderer, weatherBodyFont, "Night", lx, ly, GREY);

        int scaleW = textWidth(weatherTextFont, "100%") + m;
        int gx0 = area.x;
        int gx1 = area.x + area.w - scaleW;
        int gy0 = area.y + bodyH + 2 * m + textH;   // room for the temperature labels
        int gy1 = area.y + area.h - 2 * textH - m;  // room for times and days
        float tMin = 1e9f, tMax = -1e9f;
        int iMin = 0, iMax = 0;
        for(int i = 0; i < n; i++) {
            if(w.hours[i].temp < tMin) { tMin = w.hours[i].temp; iMin = i; }
            if(w.hours[i].temp > tMax) { tMax = w.hours[i].temp; iMax = i; }
        }
        float lo = tMin - 1, hi = tMax + 1;
        if(hi - lo < 6) {
            float mid = (hi + lo) / 2;
            lo = mid - 3;
            hi = mid + 3;
        }
        auto xAt = [&](int i) { return gx0 + (gx1 - gx0) * i / std::max(1, n - 1); };
        auto yAt = [&](float t) { return gy1 - (int)((gy1 - gy0) * (t - lo) / (hi - lo)); };

        for(int i = 0; i < n - 1; i++) {
            if(!w.hours[i].isDay) {
                boxRGBA(renderer, xAt(i), gy0, xAt(i + 1), gy1, 30, 30, 38, 255);
            }
        }
        // rain scale on the right
        for(int pct = 0; pct <= 100; pct += 50) {
            int y = gy1 - (gy1 - gy0) * pct / 100;
            lineRGBA(renderer, gx0, y, gx1, y, PANEL_EDGE.r, PANEL_EDGE.g, PANEL_EDGE.b, 255);
            putText(renderer, weatherTextFont, std::to_string(pct) + "%", gx1 + m / 2, y - textH / 2, TEAL);
        }
        int bw = std::max(1, (gx1 - gx0) / n - 1);
        for(int i = 0; i < n; i++) {
            if(w.hours[i].precipProb > 0) {
                int bh = (gy1 - gy0) * w.hours[i].precipProb / 100;
                boxRGBA(renderer, xAt(i), gy1 - bh, xAt(i) + bw, gy1, 24, 110, 96, 255);
            }
        }
        int width = std::max(2, H / 300);
        for(int i = 1; i < n; i++) {
            thickLineRGBA(renderer, xAt(i - 1), yAt(w.hours[i - 1].temp), xAt(i), yAt(w.hours[i].temp), width, 255, 255, 255, 255);
        }

        // label the warmest and coldest points on the line, plus now
        auto mark = [&](int i, bool above) {
            int x = xAt(i), y = yAt(w.hours[i].temp);
            filledCircleRGBA(renderer, x, y, width * 2, 255, 255, 255, 255);
            int tw = tempWidth(weatherTextFont, w.hours[i].temp);
            int tx = std::max(gx0, std::min(gx1 - tw, x - tw / 2));
            int ty = above ? y - textH - width * 2 : y + width * 2;
            putTemp(renderer, weatherTextFont, w.hours[i].temp, tx, ty, WHITE);
        };
        mark(0, w.hours[0].temp >= (tMin + tMax) / 2);
        if(iMax > 2) mark(iMax, true);
        if(iMin > 2 && iMin != iMax) mark(iMin, false);

        // times every 6 hours; day names at midnight
        for(int i = 0; i < n; i++) {
            struct tm lt = *localtime(&w.hours[i].time);
            if(lt.tm_hour % 6 != 0 || i == 0) {
                continue;
            }
            int x = xAt(i);
            lineRGBA(renderer, x, gy1, x, gy1 + m / 2, GREY.r, GREY.g, GREY.b, 255);
            char hh[8];
            strftime(hh, sizeof(hh), "%H:%M", &lt);
            int tw = textWidth(weatherTextFont, hh);
            if(x - tw / 2 >= gx0 && x + tw / 2 <= gx1 + scaleW) {
                putText(renderer, weatherTextFont, hh, x - tw / 2, gy1 + m / 2, GREY);
            }
            if(lt.tm_hour == 0) {
                char day[16];
                strftime(day, sizeof(day), "%A", &lt);
                lineRGBA(renderer, x, gy0, x, gy1, GREY.r, GREY.g, GREY.b, 255);
                putText(renderer, weatherTextFont, day, std::min(x + m / 2, gx1 - textWidth(weatherTextFont, day)), gy1 + m / 2 + textH, WHITE);
            }
        }
        putText(renderer, weatherTextFont, "Now", gx0, gy1 + m / 2, WHITE);
        break;
    }

    case SEG_WEEK: {
        int cols = std::max(1, (int)w.days.size());
        int colW = area.w / cols;
        int textBlock = fontHeight(weatherHeadFont) + fontHeight(weatherTextFont) + fontHeight(weatherBigFont) + fontHeight(weatherHeadFont) +
                        fontHeight(weatherBodyFont) + fontHeight(weatherTextFont);
        int glyphPx = std::max(1, std::min(colW / 19, (area.h - textBlock - 2 * m) / 22));
        int blockH = textBlock + 22 * glyphPx;
        int top = area.y + std::max(0, (area.h - blockH) / 2);
        for(int i = 0; i < (int)w.days.size(); i++) {
            const WeatherDay &d = w.days[i];
            int cx = area.x + colW * i + colW / 2;
            char day[8];
            strftime(day, sizeof(day), "%a", localtime(&d.date));
            putCentred(renderer, weatherHeadFont, i == 0 ? "Today" : day, cx, top, WHITE);
            char date[16];
            strftime(date, sizeof(date), "%d %b", localtime(&d.date));
            putCentred(renderer, weatherTextFont, date, cx, top + fontHeight(weatherHeadFont), GREY);
            int gy = top + fontHeight(weatherHeadFont) + fontHeight(weatherTextFont) + 11 * glyphPx;
            drawWeatherGlyph(d.code, true, cx, gy, glyphPx, anim);
            int ty = gy + 11 * glyphPx;
            putTempCentred(renderer, weatherBigFont, d.tempMax, cx, ty, WHITE);
            ty += fontHeight(weatherBigFont);
            putTempCentred(renderer, weatherHeadFont, d.tempMin, cx, ty, GREY);
            ty += fontHeight(weatherHeadFont);
            if(d.precipProb >= 0) {
                putCentred(renderer, weatherBodyFont, std::to_string(d.precipProb) + "%", cx, ty, TEAL);
            }
            ty += fontHeight(weatherBodyFont);
            putCentred(renderer, weatherTextFont, compass(d.windDir) + " " + std::to_string((int)lroundf(d.windMph)), cx, ty, GREY);
        }
        break;
    }

    case SEG_SUN_MOON: {
        int half = area.w / 2;
        int textH = 2 * fontHeight(weatherHeadFont) + m + 2 * fontHeight(weatherTextFont);
        int artH = std::max(16, std::min(area.h - textH - m, half * 3 / 4));  // sun glyph / moon disc size
        int top = area.y + std::max(0, (area.h - artH - textH - m) / 2);

        // sun: today's sunrise and sunset, day length, change from tomorrow
        if(!w.days.empty()) {
            const WeatherDay &today = w.days[0];
            int glyphPx = std::max(1, artH / 17);
            int cx = area.x + half / 2;
            drawWeatherGlyph(0, true, cx, top + artH / 2, glyphPx, anim);
            int ty = top + artH + m;
            putCentred(renderer, weatherHeadFont, "Sunrise " + clockString(today.sunrise), cx, ty, WHITE);
            ty += fontHeight(weatherHeadFont);
            putCentred(renderer, weatherHeadFont, "Sunset " + clockString(today.sunset), cx, ty, WHITE);
            ty += fontHeight(weatherHeadFont) + m;
            long day = (long)(today.sunset - today.sunrise);
            char len[48];
            snprintf(len, sizeof(len), "%ldh %02ldm of daylight", day / 3600, (day % 3600) / 60);
            putCentred(renderer, weatherTextFont, len, cx, ty, GREY);
            ty += fontHeight(weatherTextFont);
            if(w.days.size() > 1) {
                long change = (long)(w.days[1].sunset - w.days[1].sunrise) - day;
                char ch[48];
                snprintf(ch, sizeof(ch), "%s%ldm %02lds tomorrow", change < 0 ? "-" : "+", labs(change) / 60, labs(change) % 60);
                putCentred(renderer, weatherTextFont, ch, cx, ty, change < 0 ? GREY : TEAL);
            }
        }
        // moon: phase disc, name, illumination, next full and new moon
        {
            double p = moonPhase(nowWall);
            int cells = 20;
            int px = std::max(1, artH / cells);
            int cx = area.x + half + half / 2;
            drawMoonDisc(renderer, cx, top + artH / 2, cells, px, p);
            int ty = top + artH + m;
            putCentred(renderer, weatherHeadFont, moonPhaseName(p), cx, ty, WHITE);
            ty += fontHeight(weatherHeadFont);
            int lit = (int)lround((1.0 - cos(2.0 * M_PI * p)) / 2.0 * 100.0);
            putCentred(renderer, weatherBodyFont, std::to_string(lit) + "% lit", cx, ty, GREY);  // Heavy has no '%'
            ty += fontHeight(weatherBodyFont) + m;
            char nextFull[32], nextNew[32];
            time_t full = nextPhase(nowWall, 0.5), nw = nextPhase(nowWall, 0.0);
            strftime(nextFull, sizeof(nextFull), "Full %a %d %b", localtime(&full));
            strftime(nextNew, sizeof(nextNew), "New %a %d %b", localtime(&nw));
            putCentred(renderer, weatherTextFont, nextFull, cx, ty, WHITE);
            ty += fontHeight(weatherTextFont);
            putCentred(renderer, weatherTextFont, nextNew, cx, ty, WHITE);
        }
        break;
    }

    case SEG_RADAR:
        // drawn under the header above; nothing else here
        break;
    }
}

// Radar segment: the region around home with the last two hours of radar
// looping over a plain base map, and a timeline of the frames.
// A scrolling ticker line, rendered once to a texture and slid along.
void View::drawTicker(const std::string &text, int y, float anim) {
    TTF_Font *font = weatherBodyFont;
    if(!font) {
        return;
    }
    if(text != tickerText || !tickerTexture) {
        if(tickerTexture) {
            SDL_DestroyTexture(tickerTexture);
            tickerTexture = NULL;
        }
        tickerText = text;
        SDL_Surface *fg = TTF_RenderUTF8_Solid(font, text.c_str(), {255, 255, 255, 255});
        SDL_Surface *bg = TTF_RenderUTF8_Solid(font, text.c_str(), {0, 0, 0, 255});
        if(fg && bg) {
            int off = std::max(1, TTF_FontHeight(font) / 14);
            SDL_Surface *both = SDL_CreateRGBSurfaceWithFormat(0, fg->w + off, fg->h + off, 32, SDL_PIXELFORMAT_ARGB8888);
            if(both) {
                SDL_FillRect(both, NULL, SDL_MapRGBA(both->format, 0, 0, 0, 0));
                SDL_Rect r = {off, off, bg->w, bg->h};
                SDL_BlitSurface(bg, NULL, both, &r);
                SDL_BlitSurface(fg, NULL, both, NULL);
                tickerTexture = SDL_CreateTextureFromSurface(renderer, both);
                tickerW = both->w;
                tickerH = both->h;
                SDL_FreeSurface(both);
            }
        }
        if(fg) SDL_FreeSurface(fg);
        if(bg) SDL_FreeSurface(bg);
    }
    if(!tickerTexture) {
        return;
    }
    int gap = textWidth(font, "      ");
    float speed = screen_width / 9.0f;  // crosses the screen in ~9 s
    int span = tickerW + gap;
    int x = -(int)fmodf(anim * speed, (float)span);
    for(; x < screen_width; x += span) {
        SDL_Rect dest = {x, y, tickerW, tickerH};
        SDL_RenderCopy(renderer, tickerTexture, NULL, &dest);
    }
}

// Base map for the regional and radar segments, drawn once into a texture
// (it's ~a million points, too many to redraw every frame on a Pi) and
// redrawn only when the map data finishes loading. Uses the projection
// currently set on the view.
void View::drawBackdrop(int slot, SDL_Color color) {
    if(!backdropTexture[slot] || backdropLoaded[slot] != map.loaded) {
        if(!backdropTexture[slot]) {
            backdropTexture[slot] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, screen_width, screen_height);
            if(!backdropTexture[slot]) {
                return;
            }
        }
        SDL_SetRenderTarget(renderer, backdropTexture[slot]);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        float screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max;
        latLonFromScreenCoords(&screen_lat_min, &screen_lon_min, 0, 0);
        latLonFromScreenCoords(&screen_lat_max, &screen_lon_max, screen_width, screen_height);
        drawLinesRecursive(&(map.root), std::min(screen_lat_min, screen_lat_max), std::max(screen_lat_min, screen_lat_max),
                           screen_lon_min, screen_lon_max, color);
        SDL_SetRenderTarget(renderer, NULL);
        backdropLoaded[slot] = map.loaded;
    }
    SDL_RenderCopy(renderer, backdropTexture[slot], NULL, NULL);
}

// Regional map: nearby cities with their temperature and conditions.
void View::drawWeatherRegional(const WeatherSnapshot &w, int top, int bottom, float anim) {
    float savedDist = maxDist, savedLat = centerLat, savedLon = centerLon;
    maxDist = 300.0f;
    centerLat = appData->modes.fUserLat;
    centerLon = appData->modes.fUserLon;

    drawBackdrop(0, SDL_Color{70, 70, 84, 255});

    std::vector<SDL_Rect> placed;
    int glyphPx = std::max(1, screen_height / 170);
    for(const WeatherCity &city : w.cities) {
        float dx, dy;
        int x, y;
        pxFromLonLat(&dx, &dy, city.lon, city.lat);
        screenCoords(&x, &y, dx, dy);
        int nameW = textWidth(weatherBodyFont, city.name);
        int tempH = fontHeight(weatherHeadFont);
        SDL_Rect box = {x - nameW / 2, y - fontHeight(weatherBodyFont), nameW, fontHeight(weatherBodyFont) + tempH};
        if(box.y < top || box.y + box.h > bottom || box.x < 0 || box.x + box.w > screen_width) {
            continue;
        }
        bool clash = false;
        for(const SDL_Rect &o : placed) {
            if(SDL_HasIntersection(&box, &o)) {
                clash = true;
                break;
            }
        }
        if(clash) {
            continue;
        }
        placed.push_back(box);
        filledCircleRGBA(renderer, x, y, std::max(2, glyphPx), 255, 255, 255, 255);
        putText(renderer, weatherBodyFont, city.name, x - nameW / 2, y - fontHeight(weatherBodyFont) - glyphPx, WHITE);
        int tw = putTemp(renderer, weatherHeadFont, city.temp, x - nameW / 2, y + glyphPx, ORANGE);
        drawWeatherGlyph(city.code, city.isDay, x - nameW / 2 + tw + 9 * glyphPx, y + glyphPx + tempH / 2, glyphPx, anim);
    }

    maxDist = savedDist;
    centerLat = savedLat;
    centerLon = savedLon;
}

// Light-to-heavy colour key for RainViewer's "universal blue" scheme.
void View::drawRadarLegend(int right, int top, int height) {
    const SDL_Color keys[] = {{136, 221, 238, 255}, {0, 153, 204, 255}, {0, 102, 170, 255},
                              {255, 238, 0, 255}, {255, 170, 0, 255}, {255, 68, 0, 255}, {193, 0, 0, 255}};
    int n = sizeof(keys) / sizeof(keys[0]);
    int box = std::max(4, height / 2);
    int heavyW = textWidth(weatherBodyFont, "Heavy");
    int x = right - heavyW;
    int y = top + (height - box) / 2;
    putText(renderer, weatherBodyFont, "Heavy", x, top + (height - fontHeight(weatherBodyFont)) / 2, WHITE);
    x -= n * box + box / 2;
    for(int i = 0; i < n; i++) {
        boxRGBA(renderer, x + i * box, y, x + (i + 1) * box - 1, y + box, keys[i].r, keys[i].g, keys[i].b, 255);
    }
    int lightW = textWidth(weatherBodyFont, "Light");
    putText(renderer, weatherBodyFont, "Light", x - lightW - box / 2, top + (height - fontHeight(weatherBodyFont)) / 2, WHITE);
}

void View::drawWeatherRadar(int top, int bottom) {
    if(!radar) {
        return;
    }

    // borrow the map's projection for a wide regional view
    float savedDist = maxDist, savedLat = centerLat, savedLon = centerLon;
    maxDist = 90.0f;
    centerLat = appData->modes.fUserLat;
    centerLon = appData->modes.fUserLon;

    drawBackdrop(1, SDL_Color{60, 60, 70, 255});

    time_t frameTime = 0;
    drawRadarTiles(200, true, &frameTime);

    drawPlaceNames();

    // home
    float dx, dy;
    int hx, hy;
    pxFromLonLat(&dx, &dy, centerLon, centerLat);
    screenCoords(&hx, &hy, dx, dy);
    int r = 3 * screen_uiscale;
    lineRGBA(renderer, hx - 2 * r, hy, hx + 2 * r, hy, 255, 255, 255, 255);
    lineRGBA(renderer, hx, hy - 2 * r, hx, hy + 2 * r, 255, 255, 255, 255);
    circleRGBA(renderer, hx, hy, r, 255, 255, 255, 255);

    maxDist = savedDist;
    centerLat = savedLat;
    centerLon = savedLon;

    // black band behind the header so it stays readable
    boxRGBA(renderer, 0, 0, screen_width, top - 1, 0, 0, 0, 255);

    // timeline: one tick per frame, the current one highlighted with its time
    std::vector<RadarFrameInfo> frames = radar->frames();
    if(frames.empty()) {
        return;
    }
    int m = std::max(4, screen_height / 50);
    int barY = bottom - m - fontHeight(weatherBodyFont) - m;
    int x0 = m, x1 = screen_width - m;
    boxRGBA(renderer, 0, barY - m, screen_width, bottom, 0, 0, 0, 200);
    int current = radarLoopFrame((int)frames.size());
    for(int i = 0; i < (int)frames.size(); i++) {
        int x = x0 + (x1 - x0) * i / std::max(1, (int)frames.size() - 1);
        SDL_Color c = i == current ? TEAL : GREY;
        int h = i == current ? m : m / 2;
        boxRGBA(renderer, x - 1, barY - h, x + 1, barY, c.r, c.g, c.b, 255);
    }
    int cxCur = x0 + (x1 - x0) * current / std::max(1, (int)frames.size() - 1);
    // "Latest 08:40", or how long ago the frame is: "40 min ago"
    long ago = (long)(time(NULL) - frames[current].time) / 60;
    std::string label = current == (int)frames.size() - 1 ? "Latest " + clockString(frames[current].time)
                                                          : std::to_string(ago) + " min ago";
    int lw = textWidth(weatherBodyFont, label);
    putText(renderer, weatherBodyFont, label, std::max(x0, std::min(x1 - lw, cxCur - lw / 2)), barY + m / 2, WHITE);
}
