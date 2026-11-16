// viz1090 weather page: a retro broadcast-style local forecast with pixel-art
// glyphs. Data from WeatherData (Open-Meteo).

#include "View.h"
#include "Label.h"
#include "SDL2/SDL2_gfxPrimitives.h"

#include <algorithm>
#include <cmath>

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

// draw one mask; (x, y) is the glyph's top-left, px is the pixel block size
static void drawMask(SDL_Renderer *r, const char **mask, int rows, int x, int y, int px, bool darkCloud) {
    for(int row = 0; row < rows; row++) {
        const char *line = mask[row];
        for(int col = 0; line[col]; col++) {
            if(line[col] == '.') {
                continue;
            }
            SDL_Color c = glyphColor(line[col], darkCloud);
            boxRGBA(r, x + col * px, y + row * px, x + (col + 1) * px - 1, y + (row + 1) * px - 1, c.r, c.g, c.b, 255);
        }
    }
}

#define ROWS(m) ((int)(sizeof(m) / sizeof((m)[0])))

// A WMO weather code as a 16x16-ish pixel glyph centred on (cx, cy).
void View::drawWeatherGlyph(int code, bool isDay, int cx, int cy, int px) {
    int x = cx - 8 * px;
    int y = cy - 8 * px;

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

    const char **sky = isDay ? GLYPH_SUN : GLYPH_MOON;
    int skyRows = isDay ? ROWS(GLYPH_SUN) : ROWS(GLYPH_MOON);

    if(clear) {
        drawMask(renderer, sky, skyRows, x, y, px, false);
        return;
    }
    if(fog) {
        drawMask(renderer, GLYPH_FOG, ROWS(GLYPH_FOG), x, y + 2 * px, px, false);
        return;
    }
    if(partly || showers) {
        // sun or moon peeking out behind the cloud
        drawMask(renderer, sky, skyRows, x - 3 * px, y - 4 * px, px, false);
    }

    bool precip = drizzle || sleet || rain || heavyRain || showers || snow || thunder;
    bool dark = overcast || heavyRain || thunder;
    int cloudY = precip ? y - 3 * px : y;
    int cloudX = (partly || showers) ? x + 2 * px : x;
    drawMask(renderer, GLYPH_CLOUD, ROWS(GLYPH_CLOUD), cloudX, cloudY + (partly ? 2 * px : 0), px, dark);

    int below = cloudY + 13 * px;
    if(drizzle) {
        drawMask(renderer, GLYPH_DRIZZLE, ROWS(GLYPH_DRIZZLE), cloudX, below, px, false);
    } else if(rain || showers) {
        drawMask(renderer, GLYPH_DROPS, ROWS(GLYPH_DROPS), cloudX, below, px, false);
    } else if(heavyRain) {
        drawMask(renderer, GLYPH_DROPS, ROWS(GLYPH_DROPS), cloudX, below, px, false);
        drawMask(renderer, GLYPH_DROPS, ROWS(GLYPH_DROPS), cloudX + px, below + 2 * px, px, false);
    } else if(snow) {
        drawMask(renderer, GLYPH_SNOW, ROWS(GLYPH_SNOW), cloudX, below, px, false);
    } else if(sleet) {
        drawMask(renderer, GLYPH_SLEET, ROWS(GLYPH_SLEET), cloudX, below, px, false);
    } else if(thunder) {
        drawMask(renderer, GLYPH_BOLT, ROWS(GLYPH_BOLT), cloudX, below, px, false);
    }
}

//
// Page layout
//

// same palette as the map and departures pages: black, greys, white,
// teal for data, orange for the headline
static const SDL_Color PANEL_EDGE = {64, 64, 64, 255};
static const SDL_Color TAB_ORANGE = {253, 151, 31, 255};
static const SDL_Color TEXT_PALE = {127, 127, 127, 255};
static const SDL_Color TEMP_LOW = {127, 127, 127, 255};
static const SDL_Color RAIN_BLUE = {0, 255, 174, 255};

static std::string compass(int degrees) {
    const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[((degrees + 22) / 45) % 8];
}

static std::string tempString(float t) {
    return std::to_string((int)lroundf(t));
}

// draw text, return its width
static int putText(SDL_Renderer *r, TTF_Font *font, const std::string &s, int x, int y, SDL_Color color) {
    if(!font || s.empty()) {
        return 0;
    }
    Label label;
    label.setFont(font);
    label.setColor(color);
    label.setPosition(x, y);
    label.setText(s);
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

static void drawPanel(SDL_Renderer *r, TTF_Font *tabFont, const std::string &title, int x, int y, int w, int h) {
    roundedRectangleRGBA(r, x, y, x + w, y + h, 4, PANEL_EDGE.r, PANEL_EDGE.g, PANEL_EDGE.b, 255);
    // grey title sitting on the top edge, like the board's column headings
    int th = fontHeight(tabFont);
    int tw = textWidth(tabFont, title);
    boxRGBA(r, x + th / 2, y - th / 2, x + th + tw, y + th / 2, 0, 0, 0, 255);
    putText(r, tabFont, title, x + th * 3 / 4, y - th / 2, {127, 127, 127, 255});
}

void View::drawWeather() {
    int W = screen_width;
    int H = screen_height;
    float u = H / 36.0f;
    int m = (int)(u * 0.7f);

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    WeatherSnapshot w = weather->snapshot();
    time_t nowWall = time(NULL);

    // heading like the departures board: clock, then "Weather" in orange
    char clock[16];
    strftime(clock, sizeof(clock), "%H:%M", localtime(&nowWall));
    int hx = m;
    int hy = m;
    hx += putText(renderer, transitClockFont ? transitClockFont : weatherHeadFont, clock, hx, hy, {255, 255, 255, 255}) + m;
    hx += putText(renderer, weatherHeadFont, "Weather", hx, hy, TAB_ORANGE) + m;
    char date[32];
    strftime(date, sizeof(date), "%A %d %B", localtime(&nowWall));
    int headH = std::max(fontHeight(weatherHeadFont), fontHeight(transitClockFont));
    putText(renderer, weatherTextFont, date, hx, hy + headH - fontHeight(weatherTextFont), TEXT_PALE);
    headH += m;
    lineRGBA(renderer, m, headH + m / 2, W - m, headH + m / 2, PANEL_EDGE.r, PANEL_EDGE.g, PANEL_EDGE.b, 255);

    if(!w.valid) {
        putText(renderer, weatherTextFont, "Fetching forecast... " + weather->status(), m, headH + 2 * m, TEXT_PALE);
        return;
    }

    int top = headH + (int)(1.4f * u);

    //
    // NOW
    //
    int nowW = (int)(W * 0.44f);
    int row1H = (int)(10.5f * u);
    drawPanel(renderer, weatherTabFont, "Now", m, top, nowW - m, row1H);
    {
        int glyphPx = std::max(2, (int)(u * 0.42f));
        int gx = m + (int)(0.6f * u) + 8 * glyphPx;
        int gy = top + row1H / 2;
        drawWeatherGlyph(w.code, w.isDay, gx, gy, glyphPx);

        int tx = gx + 9 * glyphPx + (int)u;
        int ty = top + (int)(1.2f * u);
        putTemp(renderer, weatherBigFont, w.temp, tx, ty, {255, 255, 255, 255});
        int y2 = ty + fontHeight(weatherBigFont);
        putText(renderer, weatherHeadFont, weatherDescription(w.code), tx, y2, TAB_ORANGE);
        y2 += fontHeight(weatherHeadFont) + (int)(0.3f * u);
        int x2 = tx + putText(renderer, weatherTextFont, "Feels ", tx, y2, TEXT_PALE);
        putTemp(renderer, weatherTextFont, w.feelsLike, x2, y2, TEXT_PALE);
        y2 += fontHeight(weatherTextFont);
        putText(renderer, weatherTextFont, "Wind " + compass(w.windDir) + " " + std::to_string((int)lroundf(w.windMph)) + " mph", tx, y2, TEXT_PALE);
        y2 += fontHeight(weatherTextFont);
        putText(renderer, weatherTextFont, "Humidity " + std::to_string(w.humidity) + "%", tx, y2, TEXT_PALE);
    }

    //
    // NEXT HOUR: 15-minute rain
    //
    int nhX = nowW + m;
    int nhW = W - nhX - m;
    drawPanel(renderer, weatherTabFont, "Next hour", nhX, top, nhW, row1H);
    {
        // one-line summary
        bool wetNow = !w.quarterHours.empty() && w.quarterHours[0].precip > 0.05f;
        std::string summary = wetNow ? weatherDescription(w.quarterHours[0].code) + " for the next hour" : "Dry for the next hour";
        for(size_t i = 1; i < w.quarterHours.size(); i++) {
            bool wet = w.quarterHours[i].precip > 0.05f;
            if(wet != wetNow) {
                char hm[8];
                strftime(hm, sizeof(hm), "%H:%M", localtime(&w.quarterHours[i].time));
                summary = (wet ? "Rain from " : "Drying up by ") + std::string(hm);
                break;
            }
        }
        putText(renderer, weatherTextFont, summary, nhX + m, top + (int)(1.0f * u), {255, 255, 255, 255});

        int cols = std::max(1, (int)w.quarterHours.size());
        int colW = (nhW - 2 * m) / cols;
        int base = top + row1H - (int)(1.6f * u);
        int maxBar = (int)(3.0f * u);
        int glyphPx = std::max(1, (int)(u * 0.13f));
        for(int i = 0; i < (int)w.quarterHours.size(); i++) {
            const WeatherSlot &s = w.quarterHours[i];
            int cx = nhX + m + colW * i + colW / 2;
            drawWeatherGlyph(s.code, s.isDay, cx, top + (int)(3.1f * u), glyphPx);
            // rain bar, 2 mm per 15 min = full height
            int bar = (int)(std::min(1.0f, s.precip / 2.0f) * maxBar);
            int bw = colW / 3;
            boxRGBA(renderer, cx - bw / 2, base - maxBar, cx + bw / 2, base, 24, 24, 28, 255);
            if(bar > 0) {
                boxRGBA(renderer, cx - bw / 2, base - bar, cx + bw / 2, base, RAIN_BLUE.r, RAIN_BLUE.g, RAIN_BLUE.b, 255);
            }
            char hm[8];
            strftime(hm, sizeof(hm), "%H:%M", localtime(&s.time));
            putText(renderer, weatherSmallFont, hm, cx - textWidth(weatherSmallFont, hm) / 2, base + (int)(0.2f * u), TEXT_PALE);
            char mm[16];
            snprintf(mm, sizeof(mm), "%.1fmm", s.precip);
            putText(renderer, weatherSmallFont, mm, cx - textWidth(weatherSmallFont, mm) / 2, base - maxBar - fontHeight(weatherSmallFont), TEXT_PALE);
        }
    }

    //
    // NEXT 12 HOURS
    //
    int row2Top = top + row1H + (int)(1.4f * u);
    int row2H = (int)(8.0f * u);
    drawPanel(renderer, weatherTabFont, "Next 12 hours", m, row2Top, W - 2 * m, row2H);
    {
        // start from the coming hour
        size_t start = 0;
        while(start < w.hours.size() && w.hours[start].time + 3600 <= nowWall) {
            start++;
        }
        int cols = 12;
        int colW = (W - 4 * m) / cols;
        int glyphPx = std::max(1, (int)(u * 0.2f));
        for(int i = 0; i < cols && start + i < w.hours.size(); i++) {
            const WeatherSlot &s = w.hours[start + i];
            int cx = 2 * m + colW * i + colW / 2;
            char hh[8];
            strftime(hh, sizeof(hh), "%H:00", localtime(&s.time));
            std::string label = i == 0 ? "Now" : hh;
            putText(renderer, weatherSmallFont, label, cx - textWidth(weatherSmallFont, label) / 2, row2Top + (int)(0.9f * u), TEXT_PALE);
            drawWeatherGlyph(s.code, s.isDay, cx, row2Top + (int)(3.6f * u), glyphPx);
            std::string t = tempString(s.temp);
            putTemp(renderer, weatherTextFont, s.temp, cx - textWidth(weatherTextFont, t) / 2, row2Top + (int)(5.3f * u), {255, 255, 255, 255});
            if(s.precipProb >= 0) {
                std::string p = std::to_string(s.precipProb) + "%";
                putText(renderer, weatherSmallFont, p, cx - textWidth(weatherSmallFont, p) / 2, row2Top + (int)(6.6f * u), RAIN_BLUE);
            }
        }
    }

    //
    // 48 HOURS: temperature line and rain chance bars
    //
    int row3Top = row2Top + row2H + (int)(1.4f * u);
    int row3H = H - row3Top - m;
    int graphW = (int)(W * 0.5f) - m;
    drawPanel(renderer, weatherTabFont, "24 and 48 hours", m, row3Top, graphW, row3H);
    if(!w.hours.empty()) {
        int gx0 = m + (int)(2.6f * u);
        int gx1 = m + graphW - (int)(1.0f * u);
        int gy0 = row3Top + (int)(1.4f * u);
        int gy1 = row3Top + row3H - (int)(1.8f * u);
        float tMin = 1e9f, tMax = -1e9f;
        for(const WeatherSlot &s : w.hours) {
            tMin = std::min(tMin, s.temp);
            tMax = std::max(tMax, s.temp);
        }
        if(tMax - tMin < 4) {
            tMax += 2;
            tMin -= 2;
        }
        int n = (int)w.hours.size();
        auto xAt = [&](int i) { return gx0 + (gx1 - gx0) * i / std::max(1, n - 1); };
        auto yAt = [&](float t) { return gy1 - (int)((gy1 - gy0) * (t - tMin) / (tMax - tMin)); };

        // night shading
        for(int i = 0; i < n - 1; i++) {
            if(!w.hours[i].isDay) {
                boxRGBA(renderer, xAt(i), gy0, xAt(i + 1), gy1, 18, 18, 22, 255);
            }
        }
        // rain chance bars
        for(int i = 0; i < n; i++) {
            if(w.hours[i].precipProb > 0) {
                int bh = (gy1 - gy0) * w.hours[i].precipProb / 100;
                int bw = std::max(1, (gx1 - gx0) / n - 1);
                boxRGBA(renderer, xAt(i), gy1 - bh, xAt(i) + bw, gy1, 24, 70, 62, 255);
            }
        }
        // 24h marker
        if(n > 24) {
            int x24 = xAt(24);
            for(int y = gy0; y < gy1; y += 6) {
                lineRGBA(renderer, x24, y, x24, std::min(gy1, y + 3), 255, 230, 160, 255);
            }
        }
        // temperature line
        int width = std::max(2, (int)(u * 0.15f));
        for(int i = 1; i < n; i++) {
            thickLineRGBA(renderer, xAt(i - 1), yAt(w.hours[i - 1].temp), xAt(i), yAt(w.hours[i].temp), width, 255, 255, 255, 255);
        }
        // axis labels
        putTemp(renderer, weatherSmallFont, tMax, m + (int)(0.5f * u), gy0 - fontHeight(weatherSmallFont) / 2, TEXT_PALE);
        putTemp(renderer, weatherSmallFont, tMin, m + (int)(0.5f * u), gy1 - fontHeight(weatherSmallFont) / 2, TEXT_PALE);
        const char *marks[] = {"Now", "+12h", "+24h", "+36h", "+48h"};
        for(int k = 0; k < 5; k++) {
            int i = std::min(n - 1, k * 12);
            std::string s = marks[k];
            int x = xAt(i) - textWidth(weatherSmallFont, s) / 2;
            putText(renderer, weatherSmallFont, s, x, gy1 + (int)(0.3f * u), TEXT_PALE);
        }
    }

    //
    // THE WEEK
    //
    int weekX = m + graphW + m;
    int weekW = W - weekX - m;
    drawPanel(renderer, weatherTabFont, "The week", weekX, row3Top, weekW, row3H);
    {
        int cols = std::max(1, (int)w.days.size());
        int colW = (weekW - 2 * m) / cols;
        int glyphPx = std::max(1, (int)(u * 0.18f));
        for(int i = 0; i < (int)w.days.size(); i++) {
            const WeatherDay &d = w.days[i];
            int cx = weekX + m + colW * i + colW / 2;
            char day[8];
            strftime(day, sizeof(day), "%a", localtime(&d.date));
            std::string label = i == 0 ? "Today" : day;
            putText(renderer, weatherTextFont, label, cx - textWidth(weatherTextFont, label) / 2, row3Top + (int)(0.9f * u), {255, 255, 255, 255});
            drawWeatherGlyph(d.code, true, cx, row3Top + (int)(4.0f * u), glyphPx);
            int ty = row3Top + (int)(6.0f * u);
            std::string hi = tempString(d.tempMax);
            putTemp(renderer, weatherTextFont, d.tempMax, cx - textWidth(weatherTextFont, hi) / 2, ty, {255, 255, 255, 255});
            ty += fontHeight(weatherTextFont);
            std::string lo = tempString(d.tempMin);
            putTemp(renderer, weatherTextFont, d.tempMin, cx - textWidth(weatherTextFont, lo) / 2, ty, TEMP_LOW);
            ty += fontHeight(weatherTextFont);
            if(d.precipProb >= 0) {
                std::string p = std::to_string(d.precipProb) + "%";
                putText(renderer, weatherSmallFont, p, cx - textWidth(weatherSmallFont, p) / 2, ty, RAIN_BLUE);
            }
        }
    }
}
