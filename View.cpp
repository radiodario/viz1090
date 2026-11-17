// viz1090, a vizualizer for dump1090 ADSB output
//
// Copyright (C) 2020, Nathan Matsuda <info@nathanmatsuda.com>
// Copyright (C) 2014, Malcolm Robb <Support@ATTAvionics.com>
// Copyright (C) 2012, Salvatore Sanfilippo <antirez at gmail dot com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//  *  Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//  *  Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include "SDL2/SDL2_rotozoom.h"
#include "SDL2/SDL2_gfxPrimitives.h"
#include "SDL2/SDL_image.h"

#include "View.h"

#include "AircraftLabel.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <strings.h>
#include <thread>

// Set from a SIGUSR1 handler; the next frame is saved to SNAPSHOT_PATH.
// Lets you grab the screen remotely: pkill -USR1 viz1090
volatile sig_atomic_t viewSnapshotRequested = 0;
volatile sig_atomic_t viewNextSegmentRequested = 0;  // SIGUSR2: next weather segment
static const char *SNAPSHOT_PATH = "/tmp/viz1090-snapshot.bmp";

void View::presentFrame() {
    drawCornerTemp();
    if(viewSnapshotRequested) {
        viewSnapshotRequested = 0;
        SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, screen_width, screen_height, 32, SDL_PIXELFORMAT_ARGB8888);
        if(shot) {
            if(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0) {
                SDL_SaveBMP(shot, SNAPSHOT_PATH);
            }
            SDL_FreeSurface(shot);
        }
    }
    SDL_RenderPresent(renderer);
}

using fmilliseconds = std::chrono::duration<float, std::milli>;
using fseconds = std::chrono::duration<float>;

static std::chrono::high_resolution_clock::time_point now() {
    return std::chrono::high_resolution_clock::now();
}

static float elapsed(std::chrono::high_resolution_clock::time_point ref) {
            return (fmilliseconds {now() - ref}).count();
}

static  float elapsed_s(std::chrono::high_resolution_clock::time_point ref) {
    return  (fseconds {    now() - ref}).count();
}

static float clamp(float in, float min, float max) {
    float out = in;

    if(in < min) {
        out = min;
    }

    if(in > max)  {
        out = max;
    }

    return out;
}


static void CROSSVP(float *v, float *u, float *w) 
{                                                                       
    v[0] = u[1]*w[2] - u[2]*(w)[1];                             
    v[1] = u[2]*w[0] - u[0]*(w)[2];                             
    v[2] = u[0]*w[1] - u[1]*(w)[0];                             
}

SDL_Color setColor(uint8_t r, uint8_t g, uint8_t b) {
    SDL_Color out;
    out.r = r;
    out.g = g;
    out.b = b;
    return out;
}

float lerp(float a, float b, float factor) {
    if(factor > 1.0f) {
        factor = 1.0f;
    }

    if(factor < 0.0f) {
        factor = 0.0f;
    }

    return (1.0f - factor) * a + factor * b;
}

float lerpAngle(float a, float b, float factor) {
    float diff = fabs(b - a);
    if (diff > 180.0f)
    {
        if (b > a)
        {
            a += 360.0f;
        }
        else
        {
            b += 360.0f;
        }
    }

    float value = (a + ((b - a) * factor));

    if (value >= 0.0f && value <= 360.0f)
        return value;

    return fmod(value,360.0f);
}

SDL_Color lerpColor(SDL_Color aColor, SDL_Color bColor, float factor) {
    if(factor > 1.0f) {
        factor = 1.0f;
    }

    if(factor < 0.0f) {
        factor = 0.0f;
    }

    SDL_Color out;
    out.r = (1.0f - factor) * aColor.r + factor * bColor.r;
    out.g = (1.0f - factor) * aColor.g + factor * bColor.g;
    out.b = (1.0f - factor) * aColor.b + factor * bColor.b;

    return out;
}

int View::screenDist(float d) {
    float scale_factor = (screen_width > screen_height) ? screen_width : screen_height;
    // return round(0.95 * scale_factor * 0.5 * fabs(d) / maxDist);        
    return round(scale_factor * 0.5 * fabs(d) / maxDist);        
}

void View::pxFromLonLat(float *dx, float *dy, float lon, float lat) {
    if(!lon || !lat) {
        *dx = 0;
        *dy = 0;
        return;
    }

    // for accurate reprojection use the extra cos term
    *dx = LATLONMULT * (lon - centerLon) * cos(((lat + centerLat)/2.0f) * M_PI / 180.0f);
    *dy = LATLONMULT * (lat - centerLat);
}

void View::latLonFromScreenCoords(float *lat, float *lon, int x, int y) {
    float scale_factor = (screen_width > screen_height) ? screen_width : screen_height;

    float dx = maxDist * (x  - (screen_width>>1)) / (0.95 * scale_factor * 0.5 );       
    float dy = maxDist * (y  - (screen_height>>1)) / (0.95 * scale_factor * 0.5 );

    *lat = 180.0f * dy / (6371.0 * M_PI) + centerLat;
    *lon = 180.0 * dx / (cos(((*lat + centerLat)/2.0f) * M_PI / 180.0f) * 6371.0 * M_PI) + centerLon;
}


void View::screenCoords(int *outX, int *outY, float dx, float dy) {
    *outX = (screen_width>>1) + ((dx>0) ? 1 : -1) * screenDist(dx);    
    *outY = (screen_height>>1) + ((dy>0) ? -1 : 1) * screenDist(dy);        
}

int View::outOfBounds(int x, int y) {
    return outOfBounds(x, y, 0, 0, screen_width, screen_height);
}

int View::outOfBounds(int x, int y, int left, int top, int right, int bottom) {
    if(x < left || x >= right || y < top || y >= bottom ) {
        return 1;
    } else {
        return 0;
    }
}

//
// Fonts should probably go in Style
//

TTF_Font* View::loadFont(const char *name, int size)
{
    TTF_Font *font = TTF_OpenFont(name, size);

    if (font == NULL)
    {
        printf("Failed to open Font %s: %s\n", name, TTF_GetError());

        exit(1);
    }

    return font;
}

void View::closeFont(TTF_Font *font)
{   
    if (font != NULL)
    {
        TTF_CloseFont(font);
    }
}


void View::font_init() {
    mapFont = loadFont("font/TerminusTTF-4.46.0.ttf", 12 * screen_uiscale);
    mapBoldFont = loadFont("font/TerminusTTF-Bold-4.46.0.ttf", 12 * screen_uiscale);    
       
    listFont = loadFont("font/TerminusTTF-4.46.0.ttf", 12 * screen_uiscale);

    messageFont = loadFont("font/TerminusTTF-Bold-4.46.0.ttf", 12 * screen_uiscale);
    labelFont = loadFont("font/TerminusTTF-Bold-4.46.0.ttf", 12 * screen_uiscale);

    mapFontWidth = 5 * screen_uiscale;
    mapFontHeight = 12 * screen_uiscale; 

    messageFontWidth = 6 * screen_uiscale;
    messageFontHeight = 12 * screen_uiscale; 

    labelFontWidth = 6 * screen_uiscale;
    labelFontHeight = 12 * screen_uiscale;

    // every other font is a multiple of uiUnit (see SDL_init)
    auto at = [this](float k) { return std::max(8, (int)lroundf(uiUnit * k)); };

    // departures board: London Underground dot-matrix typeface
    // (github.com/petykowski/London-Underground-Dot-Matrix-Typeface)
    transitFont = loadFont("font/LondonUndergroundRegular.ttf", at(transitScale));
    transitHeadFont = loadFont("font/LondonUndergroundHeavy.ttf", at(1.5f));     // page headings
    miniBoardFont = loadFont("font/LondonUndergroundRegular.ttf", at(0.6f));
    stopNameFont = loadFont("font/TerminusTTF-4.46.0.ttf", 8 * screen_uiscale);  // map labels

    // weather page
    weatherHugeFont = loadFont("font/LondonUndergroundHeavy.ttf", at(6.0f));     // "now" temperature
    weatherBigFont = loadFont("font/LondonUndergroundHeavy.ttf", at(3.0f));      // big numbers
    weatherHeadFont = loadFont("font/LondonUndergroundHeavy.ttf", at(1.2f));     // conditions, day names
    weatherTabFont = loadFont("font/LondonUndergroundHeavy.ttf", at(1.0f));
    weatherTitleFont = loadFont("font/TerminusTTF-Bold-4.46.0.ttf", at(1.5f));
    weatherBodyFont = loadFont("font/TerminusTTF-Bold-4.46.0.ttf", at(1.0f));    // body text
    weatherTextFont = loadFont("font/TerminusTTF-Bold-4.46.0.ttf", at(0.8f));    // details
    weatherSmallFont = loadFont("font/TerminusTTF-4.46.0.ttf", at(0.65f));
    if(stopNameFont) {
        int w = 0;
        TTF_SizeUTF8(stopNameFont, "M", &w, &stopNameFontHeight);
    }
    transitClockFont = loadFont("font/LondonUndergroundBold.ttf", at(1.5f));      // clock
    if(transitFont) {
        TTF_SizeUTF8(transitFont, "M", &transitFontWidth, &transitFontHeight);
    }
}



//
// SDL Utils
//

void View::SDL_init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("Could not initialize SDL: %s\n", SDL_GetError());       
        exit(1);
    }
    
    IMG_Init(IMG_INIT_PNG);

    if (TTF_Init() < 0) {
        printf("Couldn't initialize SDL TTF: %s\n", SDL_GetError());
        exit(1);
    }

    // Crosshair pointer; --hide-cursor for kiosk screens with no mouse.
    if(hideCursor) {
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_Cursor *crosshair = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
        if(crosshair) {
            SDL_SetCursor(crosshair);
        }
        SDL_ShowCursor(SDL_ENABLE);
    }

    // Render at native resolution on high-DPI (Retina) displays instead of
    // letting the OS upscale a 1x buffer.
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;

    // --fullscreen alone keeps the display's current mode; with --screensize
    // it switches the display to that mode (e.g. 640x480 for a CRT)
    if(fullscreen) {
        flags = flags | (screen_width ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    if(screen_width == 0) {
        SDL_DisplayMode DM;
        SDL_GetCurrentDisplayMode(screen_index, &DM);
        screen_width = DM.w;
        screen_height= DM.h;

        // In a desktop window, default to half the display so it fits with
        // the title bar and dock. --screensize still overrides this.
        if(!fullscreen) {
            screen_width /= 2;
            screen_height /= 2;
        }
    }

    window =  SDL_CreateWindow("viz1090",  SDL_WINDOWPOS_CENTERED_DISPLAY(screen_index),  SDL_WINDOWPOS_CENTERED_DISPLAY(screen_index), screen_width, screen_height, flags);        
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Window size is in points; drawing happens in pixels. Work in pixels
    // everywhere and scale the UI so it keeps the same physical size.
    // Measure the window as actually created: fullscreen takes the size of
    // whichever display it lands on, not the size we requested.
    int point_width = 0, point_height = 0;
    int pixel_width = 0, pixel_height = 0;
    SDL_GetWindowSize(window, &point_width, &point_height);
    SDL_GetRendererOutputSize(renderer, &pixel_width, &pixel_height);
    screen_width = pixel_width;
    screen_height = pixel_height;
    if(point_width > 0 && pixel_width > point_width) {
        pixel_ratio = (pixel_width + point_width / 2) / point_width;
        // a fullscreen window can report its pre-fullscreen size here; real displays are 1-3x
        pixel_ratio = std::max(1, std::min(3, pixel_ratio));
    }

    // One sizing system for every page, from the screen height in pixels:
    //  - uiUnit: the base text size (board rows, weather body); headings are
    //    1.5x, details smaller, big numbers larger. Height / 27 is the board
    //    size that reads well from 640x480 up to 4K.
    //  - screen_uiscale: whole-number scale for the map's pixel-font labels
    //    and icons, height / 540 (2 at 1080p).
    // --uiscale multiplies both.
    uiUnit = screen_height * uiScaleFactor / 27.0f;
    screen_uiscale = std::max(1, (int)lroundf(screen_height * uiScaleFactor / 540.0f));

    mapTexture = SDL_CreateTexture(renderer,
                               SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_TARGET,
                               screen_width, screen_height);

    mapMoved = 1;
    mapTargetLon = 0;
    mapTargetLat = 0;
    mapTargetMaxDist = 0;

    // No SDL_RenderSetLogicalSize: we always draw at the renderer's native
    // pixel size, and a logical size would make SDL rescale mouse events on
    // top of our own pixel_ratio scaling (taps landed 2x off in fullscreen).
}


//
// Status boxes -> move to separate class 
//

void View::drawStatusBox(int *left, int *top, std::string label, std::string message, SDL_Color color) {
    int labelWidth = (label.length() + ((label.length() > 0 ) ? 1 : 0)) * labelFontWidth;
    int messageWidth = (message.length() + ((message.length() > 0 ) ? 1 : 0)) * messageFontWidth;

    // boxes run right to left from the bottom-right corner: *left is the
    // right edge of the next box
    int x = *left - labelWidth - messageWidth;
    if(x < PAD) {
        *left = screen_width - PAD;
        *top = *top - messageFontHeight - PAD;
        x = *left - labelWidth - messageWidth;
    }

    // filled black background
    if(messageWidth) {
        roundedBoxRGBA(renderer, x, *top, x + labelWidth + messageWidth, *top + messageFontHeight, ROUND_RADIUS, style.buttonBackground.r, style.buttonBackground.g, style.buttonBackground.b, SDL_ALPHA_OPAQUE);
    }

    // filled label box
    if(labelWidth) {
        roundedBoxRGBA(renderer, x, *top, x + labelWidth, *top + messageFontHeight, ROUND_RADIUS,color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
    }

    // outline message box
    if(messageWidth) {
        roundedRectangleRGBA(renderer, x, *top, x + labelWidth + messageWidth, *top + messageFontHeight, ROUND_RADIUS,color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
    }

    Label currentLabel;
    currentLabel.setFont(labelFont);
    currentLabel.setColor(style.buttonBackground);
    currentLabel.setPosition(x + labelFontWidth/2, *top);
    currentLabel.setText(label);
    currentLabel.draw(renderer);
    
    currentLabel.setFont(messageFont);
    currentLabel.setColor(color);
    currentLabel.setPosition(x + labelWidth + messageFontWidth/2, *top);
    currentLabel.setText(message);
    currentLabel.draw(renderer);

    *left = x - PAD;
}

void View::drawCenteredStatusBox(std::string label, std::string message, SDL_Color color) {

    int labelWidth = (label.length() + ((label.length() > 0 ) ? 1 : 0)) * labelFontWidth;
    int messageWidth = (message.length() + ((message.length() > 0 ) ? 1 : 0)) * messageFontWidth;

    int left = (screen_width - (labelWidth + messageWidth)) / 2;
    int top = (screen_height - labelFontHeight) / 2;
	
    drawStatusBox(&left, &top, label, message, color);
}


void View::drawStatus() {

    int left = screen_width - PAD;  // status boxes sit bottom right
    int top = screen_height - messageFontHeight - PAD;

    if(fps) {
        char fps[60] = " ";
        snprintf(fps,40,"%.1f", 1000.0 / lastFrameTime);

        drawStatusBox(&left, &top, "fps", fps, style.grey_dark);
    }


    if(transit && page != PAGE_PLANES) {
        // the bus map is busy enough: plane status only with the planes
    } else if(planeFeed) {
        char strPlaneCount[10] = " ";
        snprintf(strPlaneCount, 10, "%d", appData->numVisiblePlanes);
        drawStatusBox(&left, &top, "planes", strPlaneCount, style.buttonColor);
    } else if(noAdsb) {
        // no aircraft source: nothing to report
    } else if(!appData->connected) {
        drawStatusBox(&left,&top,"init", "connecting", style.red);
    } else {    
        char strLoc[20] = " ";
        snprintf(strLoc, 20, "%3.3fN %3.3f%c", centerLat, fabs(centerLon),(centerLon > 0) ? 'E' : 'W');
        drawStatusBox(&left, &top, "loc", strLoc, style.buttonColor);   

        char strPlaneCount[10] = " ";
        snprintf(strPlaneCount, 10,"%d/%d", appData->numVisiblePlanes, appData->numPlanes);
        drawStatusBox(&left, &top, "disp", strPlaneCount, style.buttonColor);

        char strMsgRate[18] = " ";
        snprintf(strMsgRate, 18,"%.0f/s", appData->msgRate);
        drawStatusBox(&left, &top, "rate", strMsgRate, style.buttonColor);

        char strSig[18] = " ";
        snprintf(strSig, 18, "%.0f%%", 100.0 * appData->avgSig / 1024.0);
        drawStatusBox(&left, &top, "sAvg", strSig, style.buttonColor);
    }

    if(map.loaded < 100) {
	char loaded[32] = " ";
        snprintf(loaded, 32, "loading map %d%%", map.loaded);
        drawStatusBox(&left,&top,"init", loaded, style.orange);
    }
}

//
// Main drawing
//

void View::drawPlaneOffMap(int x, int y, int *returnx, int *returny, SDL_Color planeColor) {

    float arrowWidth = 6.0 * screen_uiscale;

    float inx = x - (screen_width>>1);
    float iny = y - (screen_height>>1);
    
    float outx, outy;
    outx = inx;
    outy = iny;

    if(abs(inx) > abs(y - (screen_height>>1)) *  static_cast<float>(screen_width>>1) / static_cast<float>(screen_height>>1)) { //left / right quadrants
        outx = (screen_width>>1) * ((inx > 0) ? 1.0 : -1.0);
        outy = (outx) * iny / (inx);
    } else { // up / down quadrants
        outy = screen_height * ((iny > 0) ? 0.5 : -0.5 );
        outx = (outy) * inx / (iny);
    }

    // circleRGBA (renderer,(screen_width>>1) + outx, screen_height * CENTEROFFSET + outy,50,planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);
    // thickLineRGBA(renderer,screen_width>>1,screen_height * CENTEROFFSET, (screen_width>>1) + outx, screen_height * CENTEROFFSET + outy,arrowWidth,planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    float inmag = sqrt(inx *inx + iny*iny);
    float vec[3];
    vec[0] = inx / inmag;
    vec[1] = iny /inmag;
    vec[2] = 0;

    float up[] = {0,0,1};

    float out[3];

    CROSSVP(out,vec,up);

    int x1, x2, x3, y1, y2, y3;

    // arrow 1
    x1 = (screen_width>>1) + outx - 2.0 * arrowWidth * vec[0] + round(-arrowWidth*out[0]);
    y1 = (screen_height>>1) + outy - 2.0 * arrowWidth * vec[1] + round(-arrowWidth*out[1]);
    x2 = (screen_width>>1) + outx - 2.0 * arrowWidth * vec[0] + round(arrowWidth*out[0]);
    y2 = (screen_height>>1) + outy - 2.0 * arrowWidth * vec[1] + round(arrowWidth*out[1]);
    x3 = (screen_width>>1) +  outx - arrowWidth * vec[0];
    y3 = (screen_height>>1) + outy - arrowWidth * vec[1];
    filledTrigonRGBA(renderer, x1, y1, x2, y2, x3, y3, planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    // arrow 2
    x1 = (screen_width>>1) + outx - 3.0 * arrowWidth * vec[0] + round(-arrowWidth*out[0]);
    y1 = (screen_height>>1) + outy - 3.0 * arrowWidth * vec[1] + round(-arrowWidth*out[1]);
    x2 = (screen_width>>1) + outx - 3.0 * arrowWidth * vec[0] + round(arrowWidth*out[0]);
    y2 = (screen_height>>1) + outy - 3.0 * arrowWidth * vec[1] + round(arrowWidth*out[1]);
    x3 = (screen_width>>1) +  outx - 2.0 * arrowWidth * vec[0];
    y3 = (screen_height>>1) + outy - 2.0 * arrowWidth * vec[1];
    filledTrigonRGBA(renderer, x1, y1, x2, y2, x3, y3, planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    *returnx = x3;
    *returny = y3;
}

void View::drawPlaneIcon(int x, int y, float heading, SDL_Color planeColor)
{
    float body = 8.0 * screen_uiscale;
    float wing = 6.0 * screen_uiscale;
    float wingThick = 0.5;
    float tail = 3.0 * screen_uiscale;
    float tailThick = 0.35;
    float bodyWidth = screen_uiscale;

    float vec[3];
    vec[0] = sin(heading * M_PI / 180);
    vec[1] = -cos(heading * M_PI / 180);
    vec[2] = 0;

    float up[] = {0,0,1};

    float out[3];

    CROSSVP(out,vec,up);

    int x1, x2, y1, y2;

    //body
    x1 = x + round(-bodyWidth*out[0]);
    y1 = y + round(-bodyWidth*out[1]);
    x2 = x + round(bodyWidth*out[0]);
    y2 = y + round(bodyWidth*out[1]);

    filledTrigonRGBA (renderer, x1, y1, x2, y2, x+round(-body * vec[0]), y+round(-body*vec[1]),planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);
    filledTrigonRGBA (renderer, x1, y1, x2, y2, x+round(body * vec[0]), y+round(body*vec[1]),planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    //x1 = x + round(8*vec[0]);
    //y1 = y + round(8*vec[1]);
    //x2 = x + round(16*vec[0]);
    //y2 = y + round(16*vec[1]);

    //lineRGBA(renderer,x1,y1,x2,y2, style.white.r, style.white.g, style.white.b, SDL_ALPHA_OPAQUE);

    // x1 = x + round(-body*vec[0] + bodyWidth*out[0]);
    // y1 = y + round(-body*vec[1] + bodyWidth*out[1]);
    // x2 = x + round(body*vec[0] + bodyWidth*out[0]);
    // y2 = y + round(body*vec[1] + bodyWidth*out[1]);

    // lineRGBA(renderer,x,y,x2,y2,planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    //trigonRGBA(renderer, x + round(-wing*.35*out[0]), y + round(-wing*.35*out[1]), x + round(wing*.35*out[0]), y + round(wing*.35*out[1]), x1, y1,planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);        
    // circleRGBA(renderer, x2,y2,screen_uiscale,planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    //wing
    x1 = x + round(-wing*out[0]);
    y1 = y + round(-wing*out[1]);
    x2 = x + round(wing*out[0]);
    y2 = y + round(wing*out[1]);

    filledTrigonRGBA(renderer, x1, y1, x2, y2, x+round(body*wingThick*vec[0]), y+round(body*wingThick*vec[1]),planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);

    //tail
    x1 = x + round(-body*.75*vec[0] - tail*out[0]);
    y1 = y + round(-body*.75*vec[1] - tail*out[1]);
    x2 = x + round(-body*.75*vec[0] + tail*out[0]);
    y2 = y + round(-body*.75*vec[1] + tail*out[1]);

    filledTrigonRGBA (renderer, x1, y1, x2, y2, x+round(-body*tailThick*vec[0]), y+round(-body*tailThick*vec[1]),planeColor.r,planeColor.g,planeColor.b,SDL_ALPHA_OPAQUE);
}

void View::drawTrails(int left, int top, int right, int bottom) {
    if(planeFade <= 0) {
        return;  // mid-transition: no planes, no trails
    }
    int currentX, currentY, prevX, prevY, colorVal;
    float dx, dy;   

    Aircraft *p = appData->aircraftList.head;
    int count = 0;
    while(p) {
            if(p->lonHistory.empty()) {
                p = p->next;
                continue;
            }

                std::vector<float>::iterator lon_idx = p->lonHistory.begin();
                std::vector<float>::iterator lat_idx = p->latHistory.begin();
                std::vector<float>::iterator heading_idx = p->headingHistory.begin();

                float age = 0;

                for(; std::next(lon_idx) != p->lonHistory.end(); ++lon_idx, ++lat_idx, ++heading_idx, age += 1.0) {

                    pxFromLonLat(&dx, &dy, *(std::next(lon_idx)), *(std::next(lat_idx)));
                    screenCoords(&currentX, &currentY, dx, dy);

                    pxFromLonLat(&dx, &dy, *lon_idx, *lat_idx);

                    screenCoords(&prevX, &prevY, dx, dy);
                    if(outOfBounds(currentX,currentY,left,top,right,bottom) && outOfBounds(prevX,prevY,left,top,right,bottom)) {
                        continue;
                    }


                    SDL_Color color = lerpColor({255,0,0,255}, {255,200,0,255}, age / static_cast<float>(p->lonHistory.size()));

		            color = lerpColor(color, style.planeGoneColor, elapsed_s(p->msSeen) / DISPLAY_ACTIVE);
                    color = lerpColor(color, style.black, -1.0f + (elapsed_s(p->msSeen) / DISPLAY_ACTIVE));

                    colorVal = (uint8_t)clamp(512.0 * (age / static_cast<float>(p->lonHistory.size())), 0, 255);
		  
                    lineRGBA(renderer, prevX, prevY, currentX, currentY, color.r, color.g, color.b, colorVal); 
                }

                if(elapsed_s(p->msSeen) > DISPLAY_ACTIVE) {
                    //lineRGBA(renderer, currentX-4, currentY-4, currentX+4, currentY+4, style.planeGoneColor.r, style.planeGoneColor.g, style.planeGoneColor.b, colorVal); 
                    //lineRGBA(renderer, currentX+4, currentY-4, currentX-4, currentY+4, style.planeGoneColor.r, style.planeGoneColor.g, style.planeGoneColor.b, colorVal); 
                    SDL_Color color = lerpColor(style.planeGoneColor, style.black, -1.0f + (elapsed_s(p->msSeen) / DISPLAY_ACTIVE));
                    circleRGBA(renderer, currentX, currentY, 5, color.r, color.g, color.b, colorVal); 
                }
        p = p->next;
    }
}

void View::drawScaleBars()
{
    int scalePower = 0;
    int scaleBarDist = screenDist((float)pow(10,scalePower));

    char scaleLabel[13] = "";
        
    lineRGBA(renderer,10,10,10,10*screen_uiscale,style.scaleBarColor.r, style.scaleBarColor.g, style.scaleBarColor.b, 255);

    while(scaleBarDist < screen_width) {
        lineRGBA(renderer,10+scaleBarDist,8,10+scaleBarDist,16*screen_uiscale,style.scaleBarColor.r, style.scaleBarColor.g, style.scaleBarColor.b, 255);

        if (metric) {
            snprintf(scaleLabel,13,"%dkm", static_cast<int>(pow(10,scalePower)));
        } else {
            snprintf(scaleLabel,13,"%dmi", static_cast<int>(pow(10,scalePower)));
        }

        Label currentLabel;
        currentLabel.setFont(mapFont);
        currentLabel.setColor(style.scaleBarColor);
        currentLabel.setPosition(10+scaleBarDist, 15*screen_uiscale);
        currentLabel.setText(scaleLabel);
        currentLabel.draw(renderer);
        
        scalePower++;
        scaleBarDist = screenDist(powf(10,scalePower));
    }

    scalePower--;
    scaleBarDist = screenDist(powf(10,scalePower));

    lineRGBA(renderer,10,10+5*screen_uiscale,10+scaleBarDist,10+5*screen_uiscale, style.scaleBarColor.r, style.scaleBarColor.g, style.  scaleBarColor.b, 255);
}


void View::drawLines(int left, int top, int right, int bottom, int bailTime) {
    float screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max;

    latLonFromScreenCoords(&screen_lat_min, &screen_lon_min, left, top);
    latLonFromScreenCoords(&screen_lat_max, &screen_lon_max, right, bottom);

    // OSM layers underneath; minor roads only when zoomed in enough to read them
    // each layer fades out past a zoom level, so the city view stays clean
    const float MINOR_ROADS_MAX_DIST_KM = 6.0f;
    const float MAJOR_ROADS_MAX_DIST_KM = 12.0f;
    const float RAIL_WATER_MAX_DIST_KM = 15.0f;
    if(maxDist < RAIL_WATER_MAX_DIST_KM) {
        drawOsmLayer(OSM_WATER, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, style.waterColor);
    }
    if(maxDist < MINOR_ROADS_MAX_DIST_KM) {
        drawOsmLayer(OSM_ROADS_MINOR, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, style.roadMinorColor);
    }
    if(maxDist < MAJOR_ROADS_MAX_DIST_KM) {
        drawOsmLayer(OSM_ROADS_MAJOR, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, style.roadMajorColor);
    }
    if(maxDist < RAIL_WATER_MAX_DIST_KM) {
        drawOsmLayer(OSM_RAIL, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, style.railColor);
    }

    drawLinesRecursive(&(map.root), screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, style.geoColor);

    drawLinesRecursive(&(map.airport_root), screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, style.airportColor);

    drawTrails(left, top, right, bottom);
}

// One OpenStreetMap layer from the tiles in view; points are lon,lat pairs
// with a NaN pair between polylines.
void View::drawOsmLayer(int layer, float lat_min, float lat_max, float lon_min, float lon_max, SDL_Color color) {
    float latLo = std::min(lat_min, lat_max), latHi = std::max(lat_min, lat_max);
    float lonLo = std::min(lon_min, lon_max), lonHi = std::max(lon_min, lon_max);

    for(const OsmTile *tile : osmTiles.visible(latLo, latHi, lonLo, lonHi)) {
        const std::vector<float> &pts = tile->pts[layer];
        bool havePrev = false;
        int px = 0, py = 0;
        for(size_t i = 0; i + 1 < pts.size(); i += 2) {
            if(std::isnan(pts[i])) {
                havePrev = false;
                continue;
            }
            float dx, dy;
            int x, y;
            pxFromLonLat(&dx, &dy, pts[i], pts[i + 1]);
            screenCoords(&x, &y, dx, dy);
            if(havePrev && (x != px || y != py) && !(outOfBounds(x, y) && outOfBounds(px, py))) {
                lineRGBA(renderer, px, py, x, y, color.r, color.g, color.b, 255);
            }
            px = x;
            py = y;
            havePrev = true;
        }
    }
}

void View::drawLinesRecursive(QuadTree *tree, float screen_lat_min, float screen_lat_max, float screen_lon_min, float screen_lon_max, SDL_Color color) {
    

    if(tree == NULL) {
        return;
    }

    if (tree->lat_min > screen_lat_max || screen_lat_min > tree->lat_max) {
        return; 
    }

    if (tree->lon_min > screen_lon_max || screen_lon_min > tree->lon_max) {
        return; 
    }
    
    drawLinesRecursive(tree->nw, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, color);
    
    drawLinesRecursive(tree->sw, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, color);
    
    drawLinesRecursive(tree->ne, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, color);
    
    drawLinesRecursive(tree->se, screen_lat_min, screen_lat_max, screen_lon_min, screen_lon_max, color);

    std::vector<Line*>::iterator currentLine;

    for (currentLine = tree->lines.begin(); currentLine != tree->lines.end(); ++currentLine) {
        int x1,y1,x2,y2;
        float dx,dy;
    
        pxFromLonLat(&dx, &dy, (*currentLine)->start.lon, (*currentLine)->start.lat); 
        screenCoords(&x1, &y1, dx, dy);

        pxFromLonLat(&dx, &dy, (*currentLine)->end.lon, (*currentLine)->end.lat); 
        screenCoords(&x2, &y2, dx, dy);

        if(outOfBounds(x1,y1) && outOfBounds(x2,y2)) {
            continue;
        }

        if(x1 == x2 && y1 == y2) {
            continue;
        }

        lineRGBA(renderer, x1, y1, x2, y2, color.r, color.g, color.b, 255);     
    }

    // //Debug quadtree
    // int tl_x,tl_y,tr_x,tr_y,bl_x,bl_y,br_x,br_y;
    // float dx,dy;

    // pxFromLonLat(&dx, &dy, tree->lon_min, tree->lat_min); 
    // screenCoords(&tl_x, &tl_y, dx, dy);

    // pxFromLonLat(&dx, &dy, tree->lon_max, tree->lat_min); 
    // screenCoords(&tr_x, &tr_y, dx, dy);

    // pxFromLonLat(&dx, &dy, tree->lon_min, tree->lat_max); 
    // screenCoords(&bl_x, &bl_y, dx, dy);

    // pxFromLonLat(&dx, &dy, tree->lon_max, tree->lat_max); 
    // screenCoords(&br_x, &br_y, dx, dy);    

    // lineRGBA(renderer, tl_x, tl_y, tr_x, tr_y, 50, 50, 50, 255);     
    // lineRGBA(renderer, tr_x, tr_y, br_x, br_y, 50, 50, 50, 255);     
    // lineRGBA(renderer, bl_x, bl_y, br_x, br_y, 50, 50, 50, 255);     
    // lineRGBA(renderer, tl_x, tl_y, bl_x, bl_y, 50, 50, 50, 255);  

    // // pixelRGBA(renderer,tl_x, tl_y,255,0,0,255);
    // pixelRGBA(renderer,tr_x, tr_y,0,255,0,255);
    // pixelRGBA(renderer,bl_x, bl_y,0,0,255,255);
    // pixelRGBA(renderer,br_x, br_y,255,255,0,255);

   
}

void View::drawPlaceNames() {

    //pre-generating labels in map will trade memory for TTF calls - need to compare when there are a lot of labels on screen

    Label currentLabel;    
    currentLabel.setFont(mapFont);
    currentLabel.setColor(style.geoColor);

    for(std::vector<MapLabel*>::iterator label = map.mapnames.begin(); label != map.mapnames.end(); ++label) {
        float dx, dy;
        int x,y;

        pxFromLonLat(&dx, &dy, (*label)->location.lon, (*label)->location.lat);
        screenCoords(&x, &y, dx, dy);

        if(outOfBounds(x,y)) {
            continue;
        }

        currentLabel.setText((*label)->text);
        currentLabel.setPosition(x,y);
        currentLabel.draw(renderer);
    }

    for(std::vector<MapLabel*>::iterator label = map.airportnames.begin(); label != map.airportnames.end(); ++label) {
        float dx, dy;
        int x,y;

        pxFromLonLat(&dx, &dy, (*label)->location.lon, (*label)->location.lat);
        screenCoords(&x, &y, dx, dy);

        if(outOfBounds(x,y)) {
            continue;
        }

        currentLabel.setText((*label)->text);
        currentLabel.setPosition(x,y);
        currentLabel.draw(renderer);  
    }
}

void View::drawGeography() {
    // street detail for whatever is on screen, once zoomed in far enough to show it
    const float OSM_FETCH_MAX_DIST_KM = 8.0f;
    if(maxDist < OSM_FETCH_MAX_DIST_KM) {
        float latA, lonA, latB, lonB;
        latLonFromScreenCoords(&latA, &lonA, 0, 0);
        latLonFromScreenCoords(&latB, &lonB, screen_width, screen_height);
        osmTiles.want(std::min(latA, latB), std::max(latA, latB), std::min(lonA, lonB), std::max(lonA, lonB));
    }
    if(osmTiles.poll()) {
        mapRedraw = 1;
    }

    if((mapRedraw && !mapMoved) || (mapAnimating && elapsed(lastRedraw) > 8 * FRAMETIME) ||  elapsed(lastRedraw) > 2000 || (map.loaded < 100 && elapsed(lastRedraw) > 250)) {

        SDL_SetRenderTarget(renderer, mapTexture);
        
        SDL_SetRenderDrawColor(renderer, style.backgroundColor.r, style.backgroundColor.g, style.backgroundColor.b, 255);

        SDL_RenderClear(renderer);
        
        drawLines(0, 0, screen_width, screen_height, 0);
        drawPlaceNames();

        SDL_SetRenderTarget(renderer, NULL );   

        mapMoved = 0;
        mapRedraw = 0;
        mapAnimating = 0;

        lastRedraw = now();

        currentLon = centerLon;
        currentLat = centerLat;
        currentMaxDist = maxDist;
    }
    
    SDL_SetRenderDrawColor(renderer, style.backgroundColor.r, style.backgroundColor.g, style.backgroundColor.b, 255);

    SDL_RenderClear(renderer);

    int shiftx = 0;
    int shifty = 0;

    if(mapMoved) {
        float dx, dy;
        int x1,y1, x2, y2;
        pxFromLonLat(&dx, &dy, currentLon, currentLat);
        screenCoords(&x1, &y1, dx, dy);
        pxFromLonLat(&dx, &dy, centerLon, centerLat);
        screenCoords(&x2, &y2, dx, dy);

        shiftx = x1-x2; 
        shifty = y1-y2;

        SDL_Rect dest;

        dest.x = shiftx + (screen_width / 2) * (1 - currentMaxDist / maxDist);
        dest.y = shifty + (screen_height / 2) * (1 - currentMaxDist / maxDist);
        dest.w = screen_width * currentMaxDist / maxDist;
        dest.h = screen_height * currentMaxDist / maxDist;

        //left
        if(dest.x > 0) {
           drawLines(0, 0, dest.x, screen_height, FRAMETIME / 4);    
        }        

        //top
        if(dest.y > 0) {
            drawLines(0, screen_height - dest.y, screen_width, screen_height, FRAMETIME / 4);    
        }

        //right
        if(dest.x + dest.w < screen_width) {
           drawLines(dest.x + dest.w, 0, screen_width, screen_height, FRAMETIME / 4);    
        }        

        //bottom
        if(dest.y + dest.h < screen_height) {
            drawLines(0, 0, screen_width, screen_height - dest.y - dest.h, FRAMETIME / 4);    
        }        

        //attempt rest before bailing
        //drawGeography(dest.x, screen_height - dest.y, dest.x + dest.w, screen_height - dest.y - dest.h, 1);    

        SDL_RenderCopy(renderer, mapTexture, NULL, &dest);

        mapRedraw = 1;
        mapMoved = 0;
    } else {
        SDL_RenderCopy(renderer, mapTexture, NULL, NULL);
    }
}

void View::drawPlaneText(Aircraft *p) {
    if(!p->label) {
        p->label = new AircraftLabel(p,metric,screen_width, screen_height, mapFont);
    }

    p->label->update();
    p->label->draw(renderer, (p == selectedAircraft));
}

void View::moveLabels(float dx, float dy) {
	Aircraft *p = appData->aircraftList.head;

	while(p) { 
		if(p->label) {
			p->label->move(dx,dy);
		}

		p = p->next;
	}
}


void View::resolveLabelConflicts() {
    Aircraft *p = appData->aircraftList.head;

    while(p) {
        if(p->label) {
            p->label->clearAcceleration();
        }

        p = p->next;
    }

    p = appData->aircraftList.head;

    while(p) {
        if(p->label) {
            p->label->calculateForces(appData->aircraftList.head);
        }

        p = p->next;
    }

    p = appData->aircraftList.head;

    while(p) {

        if(p->label) {
            p->label->applyForces();

            // if(p->label->getIsChanging()) {
            //     highFramerate = true;
            // }
        }
        
        p = p->next;
    }
}

void View::drawPlanes() {
    Aircraft *p = appData->aircraftList.head;
    SDL_Color planeColor;

    if(selectedAircraft) {
        mapTargetLon = selectedAircraft->lon;
        mapTargetLat = selectedAircraft->lat;             
    }

    p = appData->aircraftList.head;

    while(p) {
        if (p->lon && p->lat) {

            // if lon lat argments were not provided, start by snapping to the first plane we see
            if(centerLon == 0 && centerLat == 0) {
                mapTargetLon = p->lon;
                mapTargetLat = p->lat;
            }

            int x, y;

	    float dx, dy;
            pxFromLonLat(&dx, &dy, p->lon, p->lat);
            screenCoords(&x, &y, dx, dy);

            float age_ms = elapsed(p->created);
            if(age_ms < 500) {
                //highFramerate = true;
                float ratio = age_ms / 500.0f;
                float radius = (1.0f - ratio * ratio) * screen_width / 8;
                for(float theta = 0; theta < 2*M_PI; theta += M_PI / 4) {
                    pixelRGBA(renderer, x + radius * cos(theta), y + radius * sin(theta), style.planeColor.r, style.planeColor.g, style.planeColor.b, 255 * ratio);
                }
                // circleRGBA(renderer, x, y, 500 - age_ms, 255,255, 255, (uint8_t)(255.0 * age_ms / 500.0));   
            } else if(1000 * DISPLAY_ACTIVE - elapsed(p->msSeen) > 500) {
                if(MODES_ACFLAGS_HEADING_VALID) {
                    int usex = x;   
                    int usey = y;
                    float useHeading = static_cast<float>(p->track);

                    p->x = usex;
                    p->y = usey;

                    planeColor = lerpColor(style.planeColor, style.planeGoneColor, elapsed_s(p->msSeen) / DISPLAY_ACTIVE);

		    if(elapsed_s(p->msSeen) > DISPLAY_ACTIVE / 2) {
		    	arcRGBA(renderer, x, y, 8, 0, 360 * 2.0 * (elapsed_s(p->msSeen) / DISPLAY_ACTIVE - 0.5), planeColor.r, planeColor.g, planeColor.b, 255);
		    }
                    
                    if(p == selectedAircraft) {
                        planeColor = style.selectedColor;
                    }
                    planeColor = lerpColor(planeColor, style.backgroundColor, 1.0f - planeFade);


                    if(outOfBounds(x,y) && planeFeed) {
                        // the online feed covers a wide area: planes off the
                        // screen would just crowd its edges, so leave them out
                        p->x = x;
                        p->y = y;
                        p = p->next;
                        continue;
                    } else if(outOfBounds(x,y)) {
                        drawPlaneOffMap(x, y, &(p->x), &(p->y), planeColor);
                    } else {
                        if(elapsed(p->msSeenLatLon) < 500) {
                            //highFramerate = true;
                            circleRGBA(renderer, p->x, p->y, elapsed(p->msSeenLatLon) * screen_width / (8192), 127,127, 127, 255 - (uint8_t)(255.0 * elapsed(p->msSeenLatLon) / 500.0));   
                        
                            pxFromLonLat(&dx, &dy, p->getLastLon(), p->getLastLat());
                            screenCoords(&x, &y, dx, dy);

                            usex = lerp(x,usex,elapsed(p->msSeenLatLon) / 500.0);
                            usey = lerp(y,usey,elapsed(p->msSeenLatLon) / 500.0);
                            useHeading = lerpAngle(p->getLastHeading(),useHeading,elapsed(p->msSeenLatLon) / 500.0);
                        }

                        drawPlaneIcon(usex, usey, useHeading, planeColor);
                    }

                    if(p->altitude > 0 && planeFade >= 1) {
                        drawPlaneText(p);  // planes on the ground (altitude 0) go unlabelled
                    }
                }
            } else {
                circleRGBA(renderer, x, y, 8 * (1000 * DISPLAY_ACTIVE - elapsed(p->msSeen)) / 500, style.planeGoneColor.r, style.planeGoneColor.g, style.planeGoneColor.b, 255);
	    }
        }
        p = p->next;
    }
}

void View::animateCenterAbsolute(float x, float y) {
    float scale_factor = (screen_width > screen_height) ? screen_width : screen_height;

    float dx = -1.0 * (0.75*(double)screen_width / (double)screen_height) * (x - screen_width/2) * maxDist / (0.95 * scale_factor * 0.5);
    float dy = 1.0 * (y - screen_height/2) * maxDist / (0.95 * scale_factor * 0.5);

    moveLabels(x,y);

    float outLat = dy * (1.0/6371.0) * (180.0f / M_PI);

    float outLon = dx * (1.0/6371.0) * (180.0f / M_PI) / cos(centerLat * M_PI / 180.0f);

    mapTargetLon = centerLon - outLon;
    mapTargetLat = centerLat - outLat;

    mapTargetMaxDist = 0.25 * maxDist;

    mapMoved = 1;
    highFramerate = true;

}

void View::moveCenterAbsolute(float x, float y) {
    float scale_factor = (screen_width > screen_height) ? screen_width : screen_height;

    float dx = -1.0 * (0.75*(double)screen_width / (double)screen_height) * (x - screen_width/2) * maxDist / (0.95 * scale_factor * 0.5);
    float dy = 1.0 * (y - screen_height/2) * maxDist / (0.95 * scale_factor * 0.5);

    moveLabels(x,y);

    float outLat = dy * (1.0/6371.0) * (180.0f / M_PI);

    float outLon = dx * (1.0/6371.0) * (180.0f / M_PI) / cos(centerLat * M_PI / 180.0f);

    centerLon += outLon;
    centerLat += outLat;

    mapTargetLon = 0;
    mapTargetLat = 0;

    mapMoved = 1;
    highFramerate = true;
}

// Zoom by factor (<1 zooms in) keeping the point under (x, y) where it is.
void View::zoomAbout(int x, int y, float factor) {
    // latLonFromScreenCoords counts y up from the bottom (fine for the
    // symmetric screen bounds it is mostly used for): flip it for a point
    float lat, lon;
    latLonFromScreenCoords(&lat, &lon, x, screen_height - y);
    maxDist = std::max(0.001f, std::min(20000.0f, maxDist * factor));
    mapTargetMaxDist = 0;

    float dx, dy;
    int nx, ny;
    pxFromLonLat(&dx, &dy, lon, lat);
    screenCoords(&nx, &ny, dx, dy);
    moveCenterRelative(x - nx, y - ny);
    mapMoved = 1;
}

void View::moveCenterRelative(float dx, float dy) {
    //
    // need to make lonlat to screen conversion class - this is just the inverse of the stuff in draw.c, without offsets
    //
        
    moveLabels(dx,dy);

    float scale_factor = (screen_width > screen_height) ? screen_width : screen_height;

    dx = -1.0 * dx * maxDist / (0.95 * scale_factor * 0.5);
    dy =  1.0 * dy * maxDist / (0.95 * scale_factor * 0.5);

    float outLat = dy * (1.0/6371.0) * (180.0f / M_PI);

    float outLon = dx * (1.0/6371.0) * (180.0f / M_PI) / cos(centerLat * M_PI / 180.0f);
    
    centerLon += outLon;
    centerLat += outLat;

    mapTargetLon = 0;
    mapTargetLat = 0;

    mapMoved = 1;
    highFramerate = true;
}

void View::zoomMapToTarget() {
    if(mapTargetMaxDist) {
        if(fabs(mapTargetMaxDist - maxDist) > 0.0001) {
            maxDist += 0.1 * (mapTargetMaxDist - maxDist);
            mapAnimating = 1;
            mapMoved = 1;
            highFramerate = true;
        } else {
            mapTargetMaxDist = 0;
        }
    }
}

void View::moveMapToTarget() {
    if(mapTargetLon && mapTargetLat) {
        if(fabs(mapTargetLon - centerLon) > 0.0001 || fabs(mapTargetLat - centerLat) > 0.0001) {
            centerLon += 0.1 * (mapTargetLon- centerLon);
            centerLat += 0.1 * (mapTargetLat - centerLat);

            mapAnimating = 1;
            mapMoved = 1;    
            highFramerate = true;
        } else {
            mapTargetLon = 0;
            mapTargetLat = 0;
        }     
    }
}

// void View::drawMouse() {
//     if(!mouseMoved) {
//         return;
//     }
    
//     if(elapsed(mouseMovedTime) > 1000) {
//         mouseMoved = false;
//         return;
//     }

//     int alpha =  (int)(255.0f - 255.0f * (float)elapsed(mouseMovedTime) / 1000.0f);

//     lineRGBA(renderer, mousex - 10 * screen_uiscale, mousey,  mousex + 10 * screen_uiscale, mousey, white.r, white.g, white.b, alpha);
//     lineRGBA(renderer, mousex,  mousey - 10 * screen_uiscale,  mousex,  mousey + 10 * screen_uiscale, white.r, white.g, white.b, alpha);
// }

void View::drawClick() {
    if(clickx && clicky) {
        highFramerate = true;

        int radius = .25 * elapsed(clickTime);
        int alpha = 128 - static_cast<int>(0.5 * elapsed(clickTime));
        if(alpha < 0 ) {
            alpha = 0;
            clickx = 0;
            clicky = 0;
        }

        filledCircleRGBA(renderer, clickx, clicky, radius,  style.clickColor.r, style.clickColor.g, style.clickColor.b, alpha);      
    }


    if(selectedAircraft) {
        // this logic should be in input, register a callback for click?

        int boxSize;
        if(elapsed(clickTime) < 300) {
            boxSize = static_cast<int>(20.0 * (1.0 - (1.0 - elapsed(clickTime) / 300.0) * cos(sqrt(elapsed(clickTime))))); 
        } else {
            boxSize = 20;
        }
        //rectangleRGBA(renderer, selectedAircraft->x - boxSize, selectedAircraft->y - boxSize, selectedAircraft->x + boxSize, selectedAircraft->y + boxSize, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);                            
        lineRGBA(renderer, selectedAircraft->x - boxSize, selectedAircraft->y - boxSize, selectedAircraft->x - boxSize/2, selectedAircraft->y - boxSize, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);
        lineRGBA(renderer, selectedAircraft->x - boxSize, selectedAircraft->y - boxSize, selectedAircraft->x - boxSize, selectedAircraft->y - boxSize/2, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);

        lineRGBA(renderer, selectedAircraft->x + boxSize, selectedAircraft->y - boxSize, selectedAircraft->x + boxSize/2, selectedAircraft->y - boxSize, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);
        lineRGBA(renderer, selectedAircraft->x + boxSize, selectedAircraft->y - boxSize, selectedAircraft->x + boxSize, selectedAircraft->y - boxSize/2, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);

        lineRGBA(renderer, selectedAircraft->x + boxSize, selectedAircraft->y + boxSize, selectedAircraft->x + boxSize/2, selectedAircraft->y + boxSize, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);
        lineRGBA(renderer, selectedAircraft->x + boxSize, selectedAircraft->y + boxSize, selectedAircraft->x + boxSize, selectedAircraft->y + boxSize/2, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);

        lineRGBA(renderer, selectedAircraft->x - boxSize, selectedAircraft->y + boxSize, selectedAircraft->x - boxSize/2, selectedAircraft->y + boxSize, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);
        lineRGBA(renderer, selectedAircraft->x - boxSize, selectedAircraft->y + boxSize, selectedAircraft->x - boxSize, selectedAircraft->y + boxSize/2, style.selectedColor.r, style.selectedColor.g, style.selectedColor.b, 255);
    } 
}

void View::registerClick(int tapcount, int x, int y) {
    if(page == PAGE_WEATHER) {
        nextWeatherSegment(1);  // tap to skip ahead
        return;
    }
    if(tapcount == 1) {
        // tap radius grows with the UI scale so it stays finger/mouse sized
        int radius = 15 * screen_uiscale;
        int radius2 = radius * radius;

        Aircraft *p = appData->aircraftList.head;
        Aircraft *selection = NULL;
        int selectionDist = radius2;

        // only planes drawn right now: p->x/p->y go stale for planes that
        // are faded out, off screen or left over from the planes page
        while(p && planeFade > 0) {
            if(x && y && p->lon && p->lat) {
                float dx, dy;
                int px, py;
                pxFromLonLat(&dx, &dy, p->lon, p->lat);
                screenCoords(&px, &py, dx, dy);
                int d = (px - x) * (px - x) + (py - y) * (py - y);
                if(!outOfBounds(px, py) && d < selectionDist) {
                    selection = p;
                    selectionDist = d;
                }
            }

            p = p->next;
        }

        // stops and stations, when they're on the map
        bool foundStop = false;
        StopMarker nearestStop;
        int stopDist = radius2;
        if(transit && page == PAGE_MAP && maxDist <= 8.0f) {
            for(const StopMarker &stop : mapStops()) {
                float dx, dy;
                int sx, sy;
                pxFromLonLat(&dx, &dy, stop.lon, stop.lat);
                screenCoords(&sx, &sy, dx, dy);
                int d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
                if(d < stopDist) {
                    stopDist = d;
                    nearestStop = stop;
                    foundStop = true;
                }
            }
        }

        // buses, as drawn this frame
        bool foundBus = false;
        std::string nearestBus;
        int busDist = radius2;
        for(const BusMarker &bus : frameBuses) {
            float dx, dy;
            int bx, by;
            pxFromLonLat(&dx, &dy, bus.lon, bus.lat);
            screenCoords(&bx, &by, dx, dy);
            int d = (bx - x) * (bx - x) + (by - y) * (by - y);
            if(d < busDist) {
                busDist = d;
                nearestBus = bus.vehicleId;
                foundBus = true;
            }
        }

        stopSelected = false;
        busSelected = false;
        selectedAircraft = NULL;

        if(foundBus && busDist <= stopDist && busDist < selectionDist) {
            selectedBusId = nearestBus;
            busSelected = true;
        } else if(foundStop && stopDist < selectionDist) {
            selectedStop = nearestStop;
            stopSelected = true;
            selectedStopHome = false;
            for(const StopMarker &m : transit->stopMarkers()) {
                selectedStopHome = selectedStopHome || m.id == selectedStop.id;
            }
            if(selectedStopHome) {
                transit->requestWalkRoute(selectedStop.lat, selectedStop.lon);
            }
        } else {
            selectedAircraft = selection;
        }
    } else if(tapcount == 2) {
        mapTargetMaxDist = 0.25 * maxDist;
        animateCenterAbsolute(x, y);
    }

    clickx = x;
    clicky = y;
    clickTime = now();
}

void View::registerMouseMove(int x, int y) {
    mouseMoved = true;
    mouseMovedTime = now();
    this->mousex = x;
    this->mousey = y;

    // aircraft debug
    // Aircraft *mouse = appData->aircraftList.find(1);
    // if(mouse != NULL) {
    //     latLonFromScreenCoords(&(mouse->lat), &(mouse->lon), x, screen_height-y);
    //     mouse->live = 1;
    // }    
    highFramerate = true;
}

//
// 
//

void View::draw() {
    drawStartTime = now();

    int targetFrameTime = 30;

    // if(highFramerate) {
    //     targetFrameTime = 15;
    // }
    // highFramerate = false;

    if (lastFrameTime < targetFrameTime) {
        SDL_Delay(static_cast<Uint32>(targetFrameTime - lastFrameTime));
    }
 
    // page rotation: per-page times if given (--page-times), else --rotate for all
    if(transit || weather || havePlanes()) {
        float seconds = usePageTimes ? pageSeconds[page] : rotateSeconds;
        if(usePageTimes && page == PAGE_PLANES && seconds <= 0) {
            seconds = 30;
        }
        if(usePageTimes && page == PAGE_WEATHER && seconds <= 0) {
            seconds = weatherCycleSeconds();  // "cycle": one full pass of the weather segments
        }
        if(seconds > 0 && std::chrono::steady_clock::now() - pageShownTime > std::chrono::milliseconds((int)(seconds * 1000))) {
            togglePage();
        }
    }

    if(page == PAGE_WEATHER) {
        if(viewNextSegmentRequested) {
            viewNextSegmentRequested = 0;
            nextWeatherSegment(1);
        }
        drawWeather();
        presentFrame();
        lastFrameTime = elapsed(drawStartTime);
        return;
    }

    if(page == PAGE_TRANSIT) {
        drawTransit();
        presentFrame();
        lastFrameTime = elapsed(drawStartTime);
        return;
    }

    fitHomeToStops();
    updateTransition();
    if(flying) {
        updateFlight();
    } else {
        moveMapToTarget();
        zoomMapToTarget();
    }

    drawGeography();
    drawRadar();
    drawScaleBars();
    updateBrowse();
    frameBuses.clear();
    if(transit && maxDist <= 8.0f) {
        frameBuses = transit->busMarkers();
        if(browse && browseLat != 0) {
            std::set<std::string> seen;
            for(const BusMarker &b : frameBuses) {
                seen.insert(b.vehicleId);
            }
            for(const BusMarker &b : browse->busMarkers()) {
                if(!seen.count(b.vehicleId)) {
                    frameBuses.push_back(b);
                }
            }
        }
    }
    if(busSelected && !selectedBus()) {
        busSelected = false;  // that bus has finished its run
    }
    updateTransitAnimation();

    drawWalkRoute();
    drawBusRoute();
    drawTransitStops();
    drawBuses();

    if(havePlanes() && planeFade > 0) {
        for(int i = 0; i < 8; i++) {
            // if(resolveLabelConflicts() <  0.001f) {            
            //     break;
            // }
            resolveLabelConflicts();
        }

        drawPlanes();  
    }

    if(transit && miniBoard && page == PAGE_MAP) {
        drawMiniBoard();
    }

    drawStatus();
    //drawMouse();
    drawClick();

    presentFrame();
    
    lastFrameTime = elapsed(drawStartTime); 
}

//
// Transit page
//

bool View::havePlanes() {
    return planeFeed || appData->connected;
}

void View::rememberHome() {
    homeLat = centerLat;
    homeLon = centerLon;
    homeDist = maxDist;
}

// Frame the local map around home and every stop within a 10-minute walk,
// so e.g. the stops at the end of the street are on screen. (All 15
// minutes' worth would zoom out ~2x.) Runs once, when walking times are in;
// --zoom is the closest it will go.
static const int FRAME_WALK_MINUTES = 10;

void View::fitHomeToStops() {
    if(homeFitted || !transit) {
        return;
    }
    std::vector<StopMarker> stops = transit->stopMarkers();
    double latMin = homeLat, latMax = homeLat, lonMin = homeLon, lonMax = homeLon;
    int used = 0;
    for(const StopMarker &m : stops) {
        if(m.walkMinutes <= 0 || m.walkMinutes > FRAME_WALK_MINUTES) {
            continue;  // not timed yet, or too far to be worth zooming out for
        }
        latMin = std::min(latMin, m.lat);
        latMax = std::max(latMax, m.lat);
        lonMin = std::min(lonMin, m.lon);
        lonMax = std::max(lonMax, m.lon);
        used++;
    }
    if(used == 0) {
        return;  // not loaded yet
    }
    homeFitted = true;
    float lat = (float)((latMin + latMax) / 2);
    float lon = (float)((lonMin + lonMax) / 2);

    // keep the stops clear of the mini departure board along the bottom
    int boardH = 0;
    TTF_Font *boardFont = miniBoardFont ? miniBoardFont : mapFont;
    if(miniBoard && boardFont) {
        int pad = PAD * pixel_ratio;
        boardH = 4 * (TTF_FontHeight(boardFont) + pad / 2) + 3 * pad;
    }
    int usableH = std::max(screen_height / 2, screen_height - boardH);

    // maxDist is half the longer screen side in km; leave room for labels
    double halfH = (latMax - latMin) / 2 * 111.195;
    double halfW = (lonMax - lonMin) / 2 * 111.195 * cos(lat * M_PI / 180.0);
    float longSide = std::max(screen_width, screen_height);
    float dist = (float)std::max(halfW * longSide / screen_width, halfH * longSide / usableH) * 1.15f;
    dist = std::max(homeDist, dist);
    // centre the stops in the area above the board: move the view's centre down
    float kmPerPx = 2.0f * dist / longSide;
    lat -= boardH / 2.0f * kmPerPx / 111.195f;
    // and a touch further south and west, so the stops at the bottom of
    // the street sit clear of the edge
    lat -= 0.06f * screen_height * kmPerPx / 111.195f;
    lon -= 0.04f * screen_width * kmPerPx / (111.195f * cosf(lat * (float)M_PI / 180.0f));
    homeLat = lat;
    homeLon = lon;
    homeDist = dist;
    if(page == PAGE_MAP && !flying) {
        flyTo(homeLat, homeLon, homeDist, 1.5f);
    }
}

void View::flyTo(float lat, float lon, float dist, float seconds) {
    flying = true;
    flyStart = std::chrono::steady_clock::now();
    flySeconds = seconds;
    flyFromLat = centerLat;
    flyFromLon = centerLon;
    flyFromDist = maxDist;
    flyToLat = lat;
    flyToLon = lon;
    flyToDist = dist;
    mapTargetLat = mapTargetLon = mapTargetMaxDist = 0;
}

void View::updateFlight() {
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - flyStart).count() / flySeconds;
    if(t >= 1) {
        t = 1;
        flying = false;
    }
    float e = t * t * (3 - 2 * t);  // ease in and out
    // the centre settles a little before the zoom does, so it lands on home
    float c = std::min(1.0f, e * 1.25f);
    centerLat = flyFromLat + (flyToLat - flyFromLat) * c;
    centerLon = flyFromLon + (flyToLon - flyFromLon) * c;
    maxDist = expf(logf(flyFromDist) + (logf(flyToDist) - logf(flyFromDist)) * e);
    mapAnimating = 1;
    mapMoved = 1;
    if(!flying) {
        mapRedraw = 1;
    }
}

void View::updateTransition() {
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - transitionStart).count();
    const float FADE_OUT_S = 0.8f, FADE_IN_S = 1.0f;
    switch(transition) {
    case TR_FADE_OUT:
        planeFade = std::max(0.0f, 1.0f - t / FADE_OUT_S);
        if(t >= FADE_OUT_S) {
            transition = TR_FLY;
            planeFade = 0;
            mapRedraw = 1;  // trails are drawn into the map texture: drop them
            flyTo(homeLat, homeLon, homeDist, 5.0f);
        }
        break;
    case TR_FLY:
        planeFade = 0;
        if(!flying) {
            transition = TR_FADE_IN;
            transitionStart = std::chrono::steady_clock::now();
        }
        break;
    case TR_FADE_IN:
        planeFade = std::min(1.0f, t / FADE_IN_S);
        if(t >= FADE_IN_S) {
            transition = TR_NONE;
            planeFade = 1;
            mapRedraw = 1;  // bring the trails back
        }
        break;
    default:
        planeFade = 1;
        break;
    }
}

void View::showPage(Page newPage) {
    if((newPage == PAGE_TRANSIT && !transit) || (newPage == PAGE_WEATHER && !weather) ||
       (newPage == PAGE_PLANES && !havePlanes())) {
        return;
    }
    if(newPage == PAGE_WEATHER && page != PAGE_WEATHER) {
        weatherSegment = 0;  // start the rotation from "Now"
        weatherSegmentShown = std::chrono::steady_clock::now();
    }
    Page previous = page;
    page = newPage;
    transition = TR_NONE;
    planeFade = 1;
    pageShownTime = std::chrono::steady_clock::now();
    if(page == PAGE_PLANES) {
        // all of London, straight away (we arrive from a full-screen page)
        flying = false;
        centerLat = planesLat;
        centerLon = planesLon;
        maxDist = planesDist;
        mapTargetLat = mapTargetLon = mapTargetMaxDist = 0;
        mapRedraw = 1;
        mapMoved = 1;
    } else if(page == PAGE_MAP) {
        mapRedraw = 1;  // the map texture is stale after time away
        if(previous == PAGE_PLANES) {
            // zoom down to the buses (see updateTransition)
            transition = TR_FADE_OUT;
            transitionStart = std::chrono::steady_clock::now();
        } else if(previous != PAGE_MAP) {
            centerLat = homeLat;
            centerLon = homeLon;
            maxDist = homeDist;
            mapMoved = 1;
        }
    }
}

// map -> departures -> weather -> planes -> map, skipping pages that aren't available
void View::togglePage() {
    Page order[] = {PAGE_MAP, PAGE_TRANSIT, PAGE_WEATHER, PAGE_PLANES};
    int count = 4;
    int current = 0;
    for(int i = 0; i < count; i++) {
        if(order[i] == page) {
            current = i;
        }
    }
    for(int step = 1; step <= count; step++) {
        Page next = order[(current + step) % count];
        if(next == PAGE_MAP || (next == PAGE_TRANSIT && transit) || (next == PAGE_WEATHER && weather) ||
           (next == PAGE_PLANES && havePlanes())) {
            showPage(next);
            return;
        }
    }
}

static SDL_Color lineBadgeColor(const Departure &dep) {
    if(dep.mode == "bus") {
        return {220, 36, 31, 255};      // London bus red
    }
    if(dep.mode == "overground") {
        return {250, 123, 5, 255};      // Overground orange
    }
    if(dep.mode == "tube") {
        return {0, 25, 168, 255};       // Underground blue
    }
    if(dep.mode == "elizabeth-line") {
        return {105, 80, 161, 255};
    }
    return {127, 127, 127, 255};
}

// The dot-matrix fonts lack some glyphs ('/', '~', brackets): swap or drop them.
static std::string dotMatrixText(TTF_Font *font, const std::string &in) {
    std::string out;
    for(char c : in) {
        if(c == ' ' || TTF_GlyphIsProvided(font, (Uint16)(unsigned char)c)) {
            out += c;
        } else if(c == '/') {
            out += '-';
        } else if(c == '~') {
            out += '.';
        }
    }
    return out;
}

// Short line label: bus routes as-is (55, N29); longer names such as the
// Overground lines become a three-letter code (Suffragette -> SUF).
static std::string shortLine(const std::string &line) {
    if(line.length() <= 4) {
        return line;
    }
    std::string code = line.substr(0, 3);
    std::transform(code.begin(), code.end(), code.begin(), ::toupper);
    return code;
}

// Stop letter from TfL's indicator ("Stop LJ" -> "LJ"); "stn" for stations.
static std::string stopCode(const std::string &indicator) {
    if(strncasecmp(indicator.c_str(), "Stop ", 5) == 0) {
        return indicator.substr(5);
    }
    return "stn";
}

static std::string fitText(const std::string &text, int chars) {
    if(chars <= 0) {
        return "";
    }
    if((int)text.length() <= chars) {
        return text;
    }
    if(chars <= 1) {
        return text.substr(0, chars);
    }
    return text.substr(0, chars - 1) + ".";
}

// Draw text so it ends at x (right-aligned), for number columns.
static void rightText(SDL_Renderer *renderer, Label &label, TTF_Font *font, const std::string &str, int x, int y) {
    int w = 0, h = 0;
    TTF_SizeUTF8(font, str.c_str(), &w, &h);
    label.setPosition(x - w, y);
    label.setText(str);
    label.draw(renderer);
}

void View::drawTransit() {
    SDL_SetRenderDrawColor(renderer, style.backgroundColor.r, style.backgroundColor.g, style.backgroundColor.b, 255);
    SDL_RenderClear(renderer);

    TTF_Font *font = transitFont ? transitFont : messageFont;
    // The dot-matrix font is proportional: size columns by the average width of
    // real text rather than "M", or names get cut short far too early.
    int charW = transitFont ? transitFontWidth : messageFontWidth;
    if(transitFont) {
        const char *sample = "King's Cross St Pancras 390 Euston Road";
        int sw = 0, sh = 0;
        TTF_SizeUTF8(transitFont, sample, &sw, &sh);
        charW = std::max(1, sw / (int)strlen(sample));
    }
    int fontH = transitFont ? transitFontHeight : messageFontHeight;

    Label text;
    text.setFont(font);

    // The dot-matrix font lacks some glyphs ('/', '~', brackets): swap or drop them.
    auto dm = [font](const std::string &in) { return dotMatrixText(font, in); };

    int pad = PAD * pixel_ratio;
    int rowH = fontH + pad;
    int left = 2 * pad;
    int totalChars = (screen_width - 4 * pad) / charW;

    // header: clock (Bold) and DEPARTURES (Heavy), then the feed status
    char clock[16];
    time_t nowWall = time(NULL);
    strftime(clock, sizeof(clock), "%H:%M", localtime(&nowWall));

    int top = 2 * pad;
    int headH = fontH;
    int x = left;

    auto drawIn = [&](TTF_Font *f, const std::string &str, SDL_Color color) {
        if(!f) {
            f = font;
        }
        Label head;
        head.setFont(f);
        head.setColor(color);
        head.setPosition(x, top);
        head.setText(str);
        head.draw(renderer);
        int w = 0, h = 0;
        TTF_SizeUTF8(f, str.c_str(), &w, &h);
        x += w + 2 * charW;
        headH = std::max(headH, h);
    };

    drawIn(transitClockFont, clock, style.white);
    drawIn(transitHeadFont, "Departures", style.orange);

    top += headH + 2 * pad;

    // columns, in characters: # LINE stop-code TO FROM WALK MINS (the stop code has no heading)
    const int badgeChars = 5;   // "N29", "SUF"
    const int codeChars = 4;    // stop letter, e.g. "LJ"
    const int walkChars = 4;    // "10m"
    // number columns measured from real digits so they can be right-aligned
    int digitsW = 0, digitsH = 0;
    TTF_SizeUTF8(font, "88", &digitsW, &digitsH);
    int minsSlot = digitsW + charW;             // one arrival time plus a small gap
    int walkW = 0;
    TTF_SizeUTF8(font, "88m", &walkW, &digitsH);
    const int dueChars = (3 * minsSlot) / charW + 1;
    int flexible = totalChars - 3 - badgeChars - codeChars - walkChars - dueChars - 5 - 4;  // 4: direction arrow
    bool showStop = flexible >= 36;
    int destChars = showStop ? flexible * 55 / 100 : flexible;
    if(destChars > 34) {
        destChars = 34;  // keep columns together on wide screens
    }
    int stopChars = showStop ? flexible - destChars - 1 : 0;
    if(stopChars > 36) {
        stopChars = 36;
    }

    const int numChars = 3;     // row number, e.g. "12"
    int xNum = left;
    int xBadge = xNum + numChars * charW;
    int xCode = xBadge + (badgeChars + 1) * charW;
    int codeW = 0, codeH = 0;
    TTF_SizeUTF8(font, "WW", &codeW, &codeH);
    // after the code: a small arrow for the way buses leave the stop
    int arrowSize = fontH * 7 / 10;
    int xArrow = xCode + std::max(codeChars * charW, codeW + charW / 2);
    int xDest = xArrow + arrowSize + 2 * charW;
    int xStop = xDest + (destChars + 1) * charW;
    int xWalk = xStop + (showStop ? (stopChars + 1) * charW : 0);
    int xDue = xWalk + (walkChars + 1) * charW;

    // column headings
    text.setColor(style.grey);
    text.setPosition(xBadge, top); text.setText(dm("LINE")); text.draw(renderer);
    text.setPosition(xDest, top); text.setText(dm("TO")); text.draw(renderer);
    if(showStop) {
        text.setPosition(xStop, top); text.setText(dm("FROM")); text.draw(renderer);
    }
    rightText(renderer, text, font, dm("WALK"), xWalk + walkW, top);
    text.setPosition(xDue + charW, top); text.setText(dm("MINS")); text.draw(renderer);
    top += rowH;
    lineRGBA(renderer, left, top - pad / 2, screen_width - left, top - pad / 2, style.grey_dark.r, style.grey_dark.g, style.grey_dark.b, 255);

    std::vector<Departure> board = transit->snapshot();
    std::map<std::string, StopMarker> stopsById;
    for(const StopMarker &m : transit->stopMarkers()) {
        stopsById[m.id] = m;
    }

    int maxRows = (screen_height - top - rowH - 2 * pad) / rowH;
    if(maxRows < 0) {
        maxRows = 0;
    }

    auto now = std::chrono::steady_clock::now();
    int shown = 0;

    for(const Departure &dep : board) {
        if(shown >= maxRows) {
            break;
        }

        // row number, like the "1st / 2nd" on station boards (right-aligned)
        text.setColor(style.grey);
        rightText(renderer, text, font, dm(std::to_string(shown + 1)), xBadge - charW, top);

        // line badge: filled box with the line name, like the status box labels
        SDL_Color badge = lineBadgeColor(dep);
        std::string lineText = shortLine(dep.line);
        int badgeW = (lineText.length() + 1) * charW;
        roundedBoxRGBA(renderer, xBadge, top, xBadge + badgeW, top + fontH, ROUND_RADIUS, badge.r, badge.g, badge.b, SDL_ALPHA_OPAQUE);
        text.setColor(style.white);
        text.setPosition(xBadge + charW / 2, top);
        text.setText(dm(lineText));
        text.draw(renderer);

        // stop letter in its own column, as on the map labels
        text.setColor(style.white);
        text.setPosition(xCode, top);
        text.setText(dm(stopCode(dep.stopIndicator)));
        text.draw(renderer);

        // which way the bus is heading, as on the map's stop arrows (no heading)
        auto stopIt = stopsById.find(dep.stopId);
        if(stopIt != stopsById.end() && !stopIt->second.station) {
            float heading = stopArrowHeading(stopIt->second);
            if(heading >= 0) {
                float hx = sinf(heading * (float)M_PI / 180.0f);
                float hy = -cosf(heading * (float)M_PI / 180.0f);
                float cx = xArrow + arrowSize / 2.0f + charW / 2.0f;
                float cy = top + fontH / 2.0f;
                float len = arrowSize / 2.0f, wid = arrowSize / 2.6f;
                filledTrigonRGBA(renderer,
                                 cx + hx * len, cy + hy * len,
                                 cx - hx * len - hy * wid, cy - hy * len + hx * wid,
                                 cx - hx * len + hy * wid, cy - hy * len - hx * wid,
                                 style.white.r, style.white.g, style.white.b, SDL_ALPHA_OPAQUE);
            }
        }

        text.setColor(style.white);
        text.setPosition(xDest, top);
        text.setText(dm(fitText(dep.destination, destChars)));
        text.draw(renderer);

        if(showStop) {
            text.setColor(style.grey);
            text.setPosition(xStop, top);
            text.setText(dm(fitText(dep.stopName, stopChars)));
            text.draw(renderer);
        }

        text.setColor(style.grey);
        rightText(renderer, text, font, dm(std::to_string(dep.walkMinutes) + "m"), xWalk + walkW, top);

        // next arrivals in tight right-aligned slots; the first turns orange when it's time to leave
        for(size_t i = 0; i < dep.arrivals.size() && i < 3; i++) {
            int mins = std::chrono::duration_cast<std::chrono::seconds>(dep.arrivals[i] - now).count() / 60;
            bool leaveNow = (i == 0 && mins - dep.walkMinutes <= 2);
            text.setColor(leaveNow ? style.orange : style.planeColor);
            rightText(renderer, text, font, dm(std::to_string(mins)), xDue + (int)(i + 1) * minsSlot, top);
        }

        top += rowH;
        shown++;
    }

    if(board.empty()) {
        text.setColor(style.grey);
        text.setPosition(left, top);
        text.setText(dm("waiting for departures..."));
        text.draw(renderer);
    } else if((int)board.size() > shown) {
        text.setColor(style.grey);
        text.setPosition(left, top);
        text.setText(dm("+" + std::to_string(board.size() - shown) + " more"));
        text.draw(renderer);
    }
}

// The next few departures in a small panel at the bottom right of the map.
void View::drawMiniBoard() {
    TTF_Font *font = miniBoardFont ? miniBoardFont : mapFont;
    if(!font) {
        return;  // fonts failed to load; drawing text would crash
    }
    const int ROWS = 4;
    const int PAGE_SECONDS = 8;

    std::vector<Departure> board = transit->snapshot();
    if(board.empty()) {
        return;
    }

    // page through the whole board, four rows at a time
    int pages = ((int)board.size() + ROWS - 1) / ROWS;
    long secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int pageIndex = (int)((secs / PAGE_SECONDS) % pages);
    size_t firstRow = pageIndex * ROWS;
    size_t rows = std::min((size_t)ROWS, board.size() - firstRow);

    int charW = 0, fontH = 0;
    TTF_SizeUTF8(font, "M", &charW, &fontH);
    int pad = PAD * pixel_ratio;
    int rowH = fontH + pad / 2;

    const int numChars = 3, badgeChars = 5, destChars = 18, stopChars = 3, minsChars = 7;
    int boxW = (numChars + badgeChars + destChars + stopChars + minsChars + 4) * charW + 2 * pad;
    int boxH = ROWS * rowH + 2 * pad;
    // bottom left (the status boxes only show on the planes page): the stops
    // sit to the south-east of home
    int left = pad;
    int top = screen_height - boxH - pad;

    roundedBoxRGBA(renderer, left, top, left + boxW, top + boxH, ROUND_RADIUS, 0, 0, 0, 220);
    roundedRectangleRGBA(renderer, left, top, left + boxW, top + boxH, ROUND_RADIUS, style.grey_dark.r, style.grey_dark.g, style.grey_dark.b, 255);

    Label text;
    text.setFont(font);
    auto now = std::chrono::steady_clock::now();
    int y = top + pad;

    // page indicator, top right of the panel
    if(pages > 1) {
        std::string pageText = std::to_string(pageIndex + 1) + "-" + std::to_string(pages);
        int w = 0, h = 0;
        TTF_SizeUTF8(font, pageText.c_str(), &w, &h);
        text.setColor(style.grey);
        text.setPosition(left + boxW - pad - w, top - h);  // just above the panel
        text.setText(dotMatrixText(font, pageText));
        text.draw(renderer);
    }

    for(size_t i = 0; i < rows; i++) {
        const Departure &dep = board[firstRow + i];
        int x = left + pad;

        text.setColor(style.grey);
        text.setPosition(x, y);
        text.setText(dotMatrixText(font, std::to_string(firstRow + i + 1)));
        text.draw(renderer);
        x += numChars * charW;

        SDL_Color badge = lineBadgeColor(dep);
        std::string lineText = shortLine(dep.line);
        roundedBoxRGBA(renderer, x, y, x + (int)(lineText.length() + 1) * charW, y + fontH, ROUND_RADIUS, badge.r, badge.g, badge.b, SDL_ALPHA_OPAQUE);
        text.setColor(style.white);
        text.setPosition(x + charW / 2, y);
        text.setText(dotMatrixText(font, lineText));
        text.draw(renderer);
        x += (badgeChars + 1) * charW;

        text.setColor(style.white);
        text.setPosition(x, y);
        text.setText(dotMatrixText(font, fitText(dep.destination, destChars)));
        text.draw(renderer);
        x += (destChars + 1) * charW;

        // stop code, as on the map labels (stations get "stn")
        std::string code = stopCode(dep.stopIndicator);
        text.setColor(style.grey);
        text.setPosition(x, y);
        text.setText(dotMatrixText(font, fitText(code, stopChars)));
        text.draw(renderer);
        x += (stopChars + 1) * charW;

        for(size_t j = 0; j < dep.arrivals.size() && j < 2; j++) {
            int mins = std::chrono::duration_cast<std::chrono::seconds>(dep.arrivals[j] - now).count() / 60;
            bool leaveNow = (j == 0 && mins - dep.walkMinutes <= 2);
            text.setColor(leaveNow ? style.orange : style.planeColor);
            text.setPosition(x, y);
            text.setText(dotMatrixText(font, std::to_string(mins)));
            text.draw(renderer);
            x += 3 * charW;
        }

        y += rowH;
    }
}

// TfL only gives a stop's direction as one of 8 compass points. Snap it to the
// bearing of the nearest OSM road, picking whichever way along the road is
// closest to TfL's heading. Cached per stop once the map has loaded.
float View::stopArrowHeading(const StopMarker &stop) {
    if(stop.heading < 0) {
        return stop.heading;
    }

    char key[64];
    snprintf(key, sizeof(key), "%.6f,%.6f", stop.lat, stop.lon);
    auto cached = stopHeadingCache.find(key);
    if(cached != stopHeadingCache.end()) {
        return cached->second;
    }

    const float SEARCH_M = 40.0f;
    const float M_PER_DEG = 111195.0f;
    float cosLat = cos(stop.lat * M_PI / 180.0f);
    float dLat = SEARCH_M / M_PER_DEG;
    float dLon = SEARCH_M / (M_PER_DEG * cosLat);

    // street tiles around the stop; until they arrive, use TfL's compass point
    std::vector<const OsmTile *> near = osmTiles.visible(stop.lat - dLat, stop.lat + dLat, stop.lon - dLon, stop.lon + dLon);
    if(near.empty()) {
        return stop.heading;
    }

    float best = SEARCH_M;
    float roadBearing = 0;
    bool foundRoad = false;
    for(const OsmTile *tile : near) {
        for(int layer : {OSM_ROADS_MAJOR, OSM_ROADS_MINOR}) {
            const std::vector<float> &pts = tile->pts[layer];
            for(size_t i = 0; i + 3 < pts.size(); i += 2) {
                if(std::isnan(pts[i]) || std::isnan(pts[i + 2])) {
                    continue;
                }
                // local metres, stop at the origin
                float ax = (pts[i] - stop.lon) * cosLat * M_PER_DEG;
                float ay = (pts[i + 1] - stop.lat) * M_PER_DEG;
                float bx = (pts[i + 2] - stop.lon) * cosLat * M_PER_DEG;
                float by = (pts[i + 3] - stop.lat) * M_PER_DEG;
                float vx = bx - ax, vy = by - ay;
                float len2 = vx * vx + vy * vy;
                if(len2 < 1.0f) {
                    continue;
                }
                float t = std::max(0.0f, std::min(1.0f, -(ax * vx + ay * vy) / len2));
                float px = ax + t * vx, py = ay + t * vy;
                float dist = sqrtf(px * px + py * py);
                if(dist < best) {
                    best = dist;
                    roadBearing = atan2f(vx, vy) * 180.0f / M_PI;  // degrees from north
                    foundRoad = true;
                }
            }
        }
    }

    float heading = stop.heading;
    if(foundRoad) {
        // the road runs both ways: take the direction nearest TfL's compass point
        float a = fmodf(roadBearing + 360.0f, 360.0f);
        float b = fmodf(a + 180.0f, 360.0f);
        auto diff = [](float x, float y) { float d = fabsf(x - y); return d > 180.0f ? 360.0f - d : d; };
        heading = diff(a, stop.heading) <= diff(b, stop.heading) ? a : b;
    }

    stopHeadingCache[key] = heading;
    return heading;
}

// The radar loop: 0.4 s per 10-minute frame, holding 2 s on the newest.
int View::radarLoopFrame(int frames) {
    if(frames <= 1) {
        return 0;
    }
    const long STEP_MS = 400, HOLD_MS = 2000;
    long cycle = frames * STEP_MS + HOLD_MS;
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % cycle;
    return (int)std::min<long>(frames - 1, ms / STEP_MS);
}

static double radarTileLon(int x, int n) {
    return x / (double)n * 360.0 - 180.0;
}

static double radarTileLat(int y, int n) {
    double k = M_PI - 2.0 * M_PI * y / n;
    return 180.0 / M_PI * atan(sinh(k));
}

// Draw the radar tiles that are on screen for the current loop frame. Tiles
// are requested on demand (like a slippy map) at a zoom that roughly matches
// the screen's scale, and decoded into textures the first time they're drawn.
void View::drawRadarTiles(Uint8 alpha, bool animate, time_t *frameTime) {
    std::vector<RadarFrameInfo> frames = radar->frames();
    if(frames.empty()) {
        return;
    }
    const RadarFrameInfo &frame = frames[animate ? radarLoopFrame((int)frames.size()) : (int)frames.size() - 1];
    if(frameTime) {
        *frameTime = frame.time;
    }

    // visible area, from the same scale the map uses
    float scale = std::max(screen_width, screen_height);
    float kmPerPx = 2.0f * maxDist / scale;
    double latHalf = (screen_height / 2.0) * kmPerPx / 111.195;
    double lonHalf = (screen_width / 2.0) * kmPerPx / (111.195 * cos(centerLat * M_PI / 180.0));
    double lonLeft = centerLon - lonHalf, lonRight = centerLon + lonHalf;
    double latTop = std::min(84.0, centerLat + latHalf), latBottom = std::max(-84.0, centerLat - latHalf);

    // zoom where a tile's pixels are about screen-pixel sized, capped by the free tier
    double wantZoom = log2(screen_width * 360.0 / (256.0 * (lonRight - lonLeft)));
    int z = std::max(2, std::min(RadarData::MAX_ZOOM, (int)lround(wantZoom)));
    int n = 1 << z;

    auto tileX = [n](double lon) { return (int)floor((lon + 180.0) / 360.0 * n); };
    auto tileY = [n](double lat) {
        double r = lat * M_PI / 180.0;
        return (int)floor((1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n);
    };
    int x0 = tileX(lonLeft), x1 = tileX(lonRight);
    int y0 = std::max(0, tileY(latTop)), y1 = std::min(n - 1, tileY(latBottom));
    if((x1 - x0 + 1) * (y1 - y0 + 1) > 64) {
        return;  // zoomed out too far for this to be useful
    }

    auto now = std::chrono::steady_clock::now();
    auto nz = [](double v) { return v == 0.0 ? 1e-7 : v; };  // pxFromLonLat treats 0 as "no position"

    for(int ty = y0; ty <= y1; ty++) {
        for(int tx = x0; tx <= x1; tx++) {
            int wx = ((tx % n) + n) % n;
            radar->want(z, wx, ty);

            // this frame's tile, or while the loop is still downloading,
            // the nearest frame that has it, so the radar is never blank
            std::string k = RadarData::key(frame.path, z, wx, ty);
            auto it = radarTextures.find(k);
            std::vector<unsigned char> png;
            if(it == radarTextures.end() && !radar->tile(frame.path, z, wx, ty, &png)) {
                bool found = false;
                for(int f = (int)frames.size() - 1; f >= 0 && !found; f--) {
                    std::string fk = RadarData::key(frames[f].path, z, wx, ty);
                    it = radarTextures.find(fk);
                    if(it != radarTextures.end()) {
                        found = true;
                    } else if(radar->tile(frames[f].path, z, wx, ty, &png)) {
                        k = fk;
                        found = true;
                    }
                }
                if(!found) {
                    continue;  // nothing downloaded for this tile yet
                }
            }
            if(it == radarTextures.end()) {
                SDL_RWops *rw = SDL_RWFromConstMem(png.data(), (int)png.size());
                SDL_Surface *surface = rw ? IMG_Load_RW(rw, 1) : NULL;
                if(!surface) {
                    continue;
                }
                SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
                SDL_FreeSurface(surface);
                if(!texture) {
                    continue;
                }
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
                RadarTexture rt = {texture, now};
                it = radarTextures.insert(std::make_pair(k, rt)).first;
            }
            it->second.lastUsed = now;

            float dx, dy;
            int px1, py1, px2, py2;
            pxFromLonLat(&dx, &dy, nz(radarTileLon(tx, n)), nz(radarTileLat(ty, n)));
            screenCoords(&px1, &py1, dx, dy);
            pxFromLonLat(&dx, &dy, nz(radarTileLon(tx + 1, n)), nz(radarTileLat(ty + 1, n)));
            screenCoords(&px2, &py2, dx, dy);
            // neighbouring tiles share exact edges; overlapping would double the alpha
            SDL_Rect dest = {px1, py1, px2 - px1, py2 - py1};
            SDL_SetTextureAlphaMod(it->second.texture, alpha);
            SDL_RenderCopy(renderer, it->second.texture, NULL, &dest);
        }
    }

    // let go of tiles that haven't been on screen for a while
    for(auto it = radarTextures.begin(); it != radarTextures.end();) {
        if(now - it->second.lastUsed > std::chrono::seconds(20)) {
            SDL_DestroyTexture(it->second.texture);
            it = radarTextures.erase(it);
        } else {
            ++it;
        }
    }
}

// Rain radar under the aircraft: the latest frame.
void View::drawRadar() {
    if(!radar || !showRadar) {
        return;
    }

    // lighter when zoomed in, where each radar pixel is a big block over the streets
    Uint8 alpha = maxDist < 3.0f ? 55 : 90;
    time_t frameTime = 0;
    drawRadarTiles(alpha, false, &frameTime);  // latest frame; the loop lives on the weather page
    if(!frameTime) {
        return;
    }

    // credit, as RainViewer asks, with the frame's time
    char when[16];
    strftime(when, sizeof(when), "%H:%M", localtime(&frameTime));
    std::string credit = std::string("Radar: RainViewer ") + when;
    TTF_Font *font = stopNameFont ? stopNameFont : mapFont;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, credit.c_str(), &w, &h);
    Label text;
    text.setFont(font);
    text.setColor(style.grey);
    text.setPosition(screen_width - w - PAD * pixel_ratio, PAD * pixel_ratio);
    text.setText(credit);
    text.draw(renderer);
}

// Ease buses towards their latest estimate, fade the selection in and out,
// and track how stale the TfL data is.
void View::updateTransitAnimation() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAnimTime).count() / 1000.0f;
    dt = std::max(0.0f, std::min(0.25f, dt));
    lastAnimTime = now;

    // buses: exponential ease, so refresh jumps become short glides
    float k = 1.0f - expf(-dt / 0.35f);
    for(BusMarker &bus : frameBuses) {
        auto it = busAnim.find(bus.vehicleId);
        if(it == busAnim.end()) {
            BusAnim a;
            a.lat = bus.lat;
            a.lon = bus.lon;
            a.firstSeen = now;
            a.lastSeen = now;
            busAnim[bus.vehicleId] = a;
            continue;
        }
        BusAnim &a = it->second;
        a.lat += (bus.lat - a.lat) * k;
        a.lon += (bus.lon - a.lon) * k;
        a.lastSeen = now;
        bus.lat = a.lat;
        bus.lon = a.lon;
    }
    for(auto it = busAnim.begin(); it != busAnim.end();) {
        if(now - it->second.lastSeen > std::chrono::seconds(60)) {
            it = busAnim.erase(it);
        } else {
            ++it;
        }
    }

    // selection fade: in over ~0.3s, and back out when deselected
    if(stopSelected) {
        fadeKind = FADE_STOP;
    } else if(busSelected) {
        fadeKind = FADE_BUS;
    }
    float target = (stopSelected || busSelected) ? 1.0f : 0.0f;
    selectionFade += (target - selectionFade) * (1.0f - expf(-dt / 0.1f));
    if(target == 0 && selectionFade < 0.01f) {
        selectionFade = 0;
        fadeKind = FADE_NONE;
    }

    // stale data drifts to grey, like aircraft we've stopped hearing
    float age = transit ? transit->dataAgeSeconds() : 0;
    transitStale = std::max(0.0f, std::min(1.0f, (age - 90.0f) / 120.0f));
}

bool View::stopDimmed(const StopMarker &stop) {
    if(fadeKind == FADE_STOP) {
        return !(stop.lat == selectedStop.lat && stop.lon == selectedStop.lon);
    }
    if(fadeKind == FADE_BUS) {
        const BusMarker *bus = selectedBus();
        return bus ? !busCallsAt(*bus, stop.id) : false;
    }
    return false;
}

bool View::busDimmed(const BusMarker &bus) {
    if(fadeKind == FADE_BUS) {
        return bus.vehicleId != selectedBusId;
    }
    if(fadeKind == FADE_STOP) {
        return !busCallsAt(bus, selectedStop.id);
    }
    return false;
}

// Apply the animated selection dimming and staleness to a transit colour.
SDL_Color View::transitColor(SDL_Color c, bool dimmed) {
    float scale = dimmed ? 1.0f - 0.67f * selectionFade : 1.0f;
    SDL_Color out = c;
    out.r = (Uint8)(c.r * scale);
    out.g = (Uint8)(c.g * scale);
    out.b = (Uint8)(c.b * scale);
    if(transitStale > 0) {
        SDL_Color grey = style.planeGoneColor;
        out.r = (Uint8)(out.r + (grey.r * scale - out.r) * transitStale);
        out.g = (Uint8)(out.g + (grey.g * scale - out.g) * transitStale);
        out.b = (Uint8)(out.b + (grey.b * scale - out.b) * transitStale);
    }
    out.a = c.a;
    return out;
}

const BusMarker *View::selectedBus() {
    for(const BusMarker &bus : frameBuses) {
        if(bus.vehicleId == selectedBusId) {
            return &bus;
        }
    }
    return NULL;
}

bool View::busCallsAt(const BusMarker &bus, const std::string &stopId) {
    for(const BusStopCall &call : bus.upcoming) {
        if(call.id == stopId) {
            return true;
        }
    }
    return false;
}

// Selected bus: its route ahead through the next stops, and a summary box.
void View::drawBusRoute() {
    const BusMarker *bus = busSelected ? selectedBus() : NULL;
    if(!bus) {
        return;
    }

    SDL_Color color = style.selectedColor;
    int width = std::max(2, screen_uiscale);
    const size_t MAX_STOPS = 12;

    float dx, dy;
    int px, py;
    pxFromLonLat(&dx, &dy, bus->lon, bus->lat);
    screenCoords(&px, &py, dx, dy);

    size_t drawn = 0;
    for(const BusStopCall &call : bus->upcoming) {
        if(!call.located) {
            continue;
        }
        int sx, sy;
        pxFromLonLat(&dx, &dy, call.lon, call.lat);
        screenCoords(&sx, &sy, dx, dy);
        thickLineRGBA(renderer, px, py, sx, sy, width, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
        filledCircleRGBA(renderer, sx, sy, width + 1, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
        px = sx;
        py = sy;
        if(++drawn >= MAX_STOPS) {
            break;
        }
    }

    // "N29 to Enfield Town  Wood Green 1m  Turnpike Lane 3m ..."
    auto now = std::chrono::steady_clock::now();
    std::string message = bus->line + " to " + bus->destination;
    for(size_t i = 0; i < bus->upcoming.size() && i < 3; i++) {
        int mins = std::chrono::duration_cast<std::chrono::seconds>(bus->upcoming[i].arrival - now).count() / 60;
        message += "  " + bus->upcoming[i].name + " " + std::to_string(std::max(0, mins)) + "m";
    }
    int maxChars = (screen_width - 2 * PAD) / messageFontWidth - 5;
    if((int)message.length() > maxChars) {
        message = message.substr(0, maxChars - 1) + "~";
    }
    int left = PAD;
    int top = screen_height - 2 * (messageFontHeight + PAD);
    drawStatusBox(&left, &top, "bus", message, color);
}

// Buses as small red rectangles along their route, with the line number.
void View::drawBuses() {
    if(!transit || frameBuses.empty()) {
        return;
    }

    float len = 4.0f * screen_uiscale;  // half length
    float wid = 1.8f * screen_uiscale;  // half width
    SDL_Color body = {220, 36, 31, 255};
    SDL_Color edge = {255, 200, 190, 255};

    Label text;
    text.setFont(stopNameFont ? stopNameFont : mapFont);
    text.setColor(style.white);

    for(const BusMarker &bus : frameBuses) {
        bool dimmed = busDimmed(bus);
        auto fade = [&](SDL_Color c) { return transitColor(c, dimmed); };
        SDL_Color fill = fade(body);
        SDL_Color outline = fade(edge);
        text.setColor(fade(style.white));

        float dx, dy;
        int x, y;
        pxFromLonLat(&dx, &dy, bus.lon, bus.lat);
        screenCoords(&x, &y, dx, dy);
        if(outOfBounds(x, y)) {
            continue;
        }

        float hx = sin(bus.heading * M_PI / 180.0f);
        float hy = -cos(bus.heading * M_PI / 180.0f);
        Sint16 vx[4], vy[4];
        float cx[4] = {len, len, -len, -len};
        float cy[4] = {-wid, wid, wid, -wid};
        for(int i = 0; i < 4; i++) {
            // rotate: along = heading, across = perpendicular
            vx[i] = (Sint16)(x + cx[i] * hx - cy[i] * hy);
            vy[i] = (Sint16)(y + cx[i] * hy + cy[i] * hx);
        }
        filledPolygonRGBA(renderer, vx, vy, 4, fill.r, fill.g, fill.b, SDL_ALPHA_OPAQUE);
        polygonRGBA(renderer, vx, vy, 4, outline.r, outline.g, outline.b, SDL_ALPHA_OPAQUE);

        // new bus: a ring of dots collapsing onto it, like a new aircraft
        auto anim = busAnim.find(bus.vehicleId);
        if(anim != busAnim.end()) {
            float age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - anim->second.firstSeen).count();
            if(age < 600.0f) {
                float ratio = age / 600.0f;
                float radius = (1.0f - ratio * ratio) * 12.0f * screen_uiscale;
                for(float theta = 0; theta < 2 * M_PI; theta += M_PI / 4) {
                    filledCircleRGBA(renderer, x + radius * cos(theta), y + radius * sin(theta), std::max(1, screen_uiscale / 2),
                                     fill.r, fill.g, fill.b, (Uint8)(255 * ratio));
                }
            }
        }

        text.setPosition(x + (int)len + screen_uiscale, y - (int)len);
        text.setText(bus.line);
        text.draw(renderer);
    }
}

// Walking route from home to the tapped stop, with its time and distance.
void View::drawWalkRoute() {
    if(!transit || !stopSelected) {
        return;
    }

    SDL_Color color = style.selectedColor;
    WalkRoute route;
    if(selectedStopHome) {
        route = transit->walkRoute(selectedStop.lat, selectedStop.lon);
    }

    if(route.state == WalkRoute::READY) {
        int width = std::max(2, screen_uiscale);
        int prevX = 0, prevY = 0;
        for(size_t i = 0; i < route.path.size(); i++) {
            float dx, dy;
            int px, py;
            pxFromLonLat(&dx, &dy, route.path[i].second, route.path[i].first);
            screenCoords(&px, &py, dx, dy);
            if(i > 0) {
                thickLineRGBA(renderer, prevX, prevY, px, py, width, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
            }
            prevX = px;
            prevY = py;
        }
    }

    // ring around the chosen stop
    float dx, dy;
    int sx, sy;
    pxFromLonLat(&dx, &dy, selectedStop.lon, selectedStop.lat);
    screenCoords(&sx, &sy, dx, dy);
    int r = 7 * screen_uiscale;
    circleRGBA(renderer, sx, sy, r, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
    circleRGBA(renderer, sx, sy, r + 1, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);

    // summary box just above the status bar
    std::string name = selectedStop.name;
    std::string id = selectedStop.indicator;
    if(strncasecmp(id.c_str(), "Stop ", 5) == 0) {
        name += " " + id.substr(5);
    }
    std::string message;
    if(route.state == WalkRoute::READY) {
        message = name + "  " + std::to_string(route.minutes) + " min  " + std::to_string(route.meters) + " m";
    } else if(!selectedStopHome) {
        message = name;  // a browsed stop: too far from home to walk
    } else if(route.state == WalkRoute::FAILED) {
        message = name + "  no route";
    } else {
        message = name + "  routing...";
    }
    int left = PAD;
    int top = screen_height - 2 * (messageFontHeight + PAD);
    drawStatusBox(&left, &top, "walk", message, color);
}

// Stops and buses follow the map: once it is panned away from home (and
// zoomed in enough to show stops), a second board looks around the middle of
// the screen, re-centring whenever the map settles somewhere new.
void View::updateBrowse() {
    if(!browse || !transit) {
        return;
    }
    const float BROWSE_MAX_DIST_KM = 3.0f;   // zoomed out further, stops are just a blob
    const float AWAY_FROM_HOME_KM = 0.7f;    // home's own stops cover ~1 km
    const int SETTLE_MS = 800;               // wait for a drag to stop

    auto km = [](double lat1, double lon1, double lat2, double lon2) {
        double dy = (lat2 - lat1) * 111.195;
        double dx = (lon2 - lon1) * 111.195 * cos(lat1 * M_PI / 180.0);
        return sqrt(dx * dx + dy * dy);
    };

    auto now = std::chrono::steady_clock::now();
    if(centerLat != browsePrevLat || centerLon != browsePrevLon) {
        browseMovedAt = now;
        browsePrevLat = centerLat;
        browsePrevLon = centerLon;
    }

    bool away = page == PAGE_MAP && !flying && maxDist <= BROWSE_MAX_DIST_KM &&
                km(centerLat, centerLon, transit->lat, transit->lon) > AWAY_FROM_HOME_KM;
    browse->setActive(away);
    if(!away) {
        browseLat = browseLon = 0;  // forget it, so coming back looks again
        return;
    }

    int radius = std::max(400, std::min(800, (int)(maxDist * 1000)));
    bool settled = now - browseMovedAt > std::chrono::milliseconds(SETTLE_MS);
    bool far = browseLat == 0 || km(centerLat, centerLon, browseLat, browseLon) * 1000 > radius / 2;
    if(settled && far) {
        browse->recentre(centerLat, centerLon, radius);
        browseLat = centerLat;
        browseLon = centerLon;
    }
}

std::vector<StopMarker> View::mapStops() {
    std::vector<StopMarker> stops = transit->stopMarkers();
    if(browse && browseLat != 0) {
        std::set<std::string> seen;
        for(const StopMarker &m : stops) {
            seen.insert(m.id);
        }
        for(const StopMarker &m : browse->stopMarkers()) {
            if(!seen.count(m.id)) {
                stops.push_back(m);
            }
        }
    }
    return stops;
}

// Bus stops and stations on the map. Labels with the next arrivals appear
// once zoomed in far enough for them not to pile up.
void View::drawTransitStops() {
    if(!transit) {
        return;
    }

    const float LABEL_MAX_DIST_KM = 3.0f;
    const float MARKER_MAX_DIST_KM = 8.0f;
    if(maxDist > MARKER_MAX_DIST_KM) {
        return;  // at city scale the stops are just a blob
    }
    bool labels = maxDist < LABEL_MAX_DIST_KM;

    int half = 2 * screen_uiscale;
    auto now = std::chrono::steady_clock::now();

    Label text;
    text.setFont(mapFont);

    // stations first, then nearest stops, so they win when labels would collide
    std::vector<StopMarker> stops = mapStops();
    auto isSelected = [this](const StopMarker &m) {
        return stopSelected && m.lat == selectedStop.lat && m.lon == selectedStop.lon;
    };
    // the selected stop first, then stations, then nearest
    std::sort(stops.begin(), stops.end(), [&](const StopMarker &a, const StopMarker &b) {
        if(isSelected(a) != isSelected(b)) {
            return isSelected(a);
        }
        if(a.station != b.station) {
            return a.station;
        }
        return a.walkMinutes < b.walkMinutes;
    });
    std::vector<SDL_Rect> placed;

    // every marker on screen, so labels keep off other stops
    std::vector<SDL_Rect> markerRects;
    for(const StopMarker &stop : stops) {
        float dx, dy;
        int x, y;
        pxFromLonLat(&dx, &dy, stop.lon, stop.lat);
        screenCoords(&x, &y, dx, dy);
        int r = (stop.station ? 3 : 2) * half + 1;
        markerRects.push_back(SDL_Rect{x - r, y - r, 2 * r, 2 * r});
    }

    for(const StopMarker &stop : stops) {
        float dx, dy;
        int x, y;
        pxFromLonLat(&dx, &dy, stop.lon, stop.lat);
        screenCoords(&x, &y, dx, dy);
        if(outOfBounds(x, y)) {
            continue;
        }

        // with a stop or bus selected, the others fade back (not out)
        bool dimmed = stopDimmed(stop);
        auto fade = [&](SDL_Color c) { return transitColor(c, dimmed); };

        SDL_Color color = fade(stop.station ? SDL_Color{250, 123, 5, 255} : SDL_Color{220, 36, 31, 255});
        float heading = stopArrowHeading(stop);
        if(heading >= 0 && !stop.station) {
            // arrow pointing the way buses leave the stop, along its road
            float hx = sin(heading * M_PI / 180.0f);
            float hy = -cos(heading * M_PI / 180.0f);
            float len = 2.5f * half;
            float wid = 1.5f * half;
            filledTrigonRGBA(renderer,
                             x + hx * len, y + hy * len,
                             x - hx * len * 0.6f - hy * wid, y - hy * len * 0.6f + hx * wid,
                             x - hx * len * 0.6f + hy * wid, y - hy * len * 0.6f - hx * wid,
                             color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
        } else if(stop.station) {
            // simplified TfL roundel: coloured ring with a navy bar across
            SDL_Color ring = fade(stop.railMode == "tube" ? SDL_Color{220, 36, 31, 255} : SDL_Color{238, 118, 35, 255});
            SDL_Color bar = fade(SDL_Color{16, 6, 159, 255});
            int r = 3 * half;
            int inner = r * 6 / 10;
            filledCircleRGBA(renderer, x, y, r, ring.r, ring.g, ring.b, SDL_ALPHA_OPAQUE);
            filledCircleRGBA(renderer, x, y, inner, 0, 0, 0, SDL_ALPHA_OPAQUE);
            int barW = r * 13 / 10;
            int barH = std::max(2, r * 4 / 10);
            boxRGBA(renderer, x - barW, y - barH / 2, x + barW, y + barH / 2, bar.r, bar.g, bar.b, SDL_ALPHA_OPAQUE);
        } else {
            boxRGBA(renderer, x - half, y - half, x + half, y + half, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
        }

        // a bus is due here: pulse an expanding ring, like an aircraft position ping
        {
            auto nowT = std::chrono::steady_clock::now();
            bool due = false;
            for(const auto &arrival : stop.next) {
                long secs = std::chrono::duration_cast<std::chrono::seconds>(arrival.second - nowT).count();
                if(secs >= -15 && secs <= 15) {
                    due = true;
                    break;
                }
            }
            if(due) {
                long ms = std::chrono::duration_cast<std::chrono::milliseconds>(nowT.time_since_epoch()).count();
                float phase = (ms % 1200) / 1200.0f;
                int rr = (int)(2 * half + phase * 5 * half);
                circleRGBA(renderer, x, y, rr, color.r, color.g, color.b, (Uint8)(220 * (1.0f - phase)));
            }
        }

        bool selected = isSelected(stop);
        if(!labels && !selected) {
            continue;
        }

        std::string id = stop.indicator;
        if(strncasecmp(id.c_str(), "Stop ", 5) == 0) {
            id = id.substr(5);
        }
        if(stop.station || id.empty()) {
            id = stop.name;
            const std::string suffix = " Rail Station";
            if(id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0) {
                id.erase(id.size() - suffix.size());
            }
        }

        // stacked like the plane labels: stop code, name, then the next two arrivals
        int smallH = stopNameFont ? stopNameFontHeight : mapFontHeight;
        bool showName = !stop.station && stopNameFont;
        std::vector<std::pair<std::string, SDL_Color>> arrivals;
        for(const auto &arrival : stop.next) {
            int mins = std::chrono::duration_cast<std::chrono::seconds>(arrival.second - now).count() / 60;
            if(mins < 0) {
                continue;
            }
            // dim the ones you can't walk to in time
            arrivals.push_back(std::make_pair(shortLine(arrival.first) + " " + std::to_string(mins) + "m",
                                              fade(mins <= stop.walkMinutes ? style.grey_dark : style.subLabelColor)));
            if(arrivals.size() >= 2) {
                break;
            }
        }
        auto widthOf = [](TTF_Font *f, const std::string &str) {
            int w = 0, h = 0;
            if(f) {
                TTF_SizeUTF8(f, str.c_str(), &w, &h);
            }
            return w;
        };
        int idW = widthOf(mapBoldFont, id);
        int nameW = showName ? widthOf(stopNameFont, stop.name) : 0;
        int labelW = std::max(idW, nameW);
        for(const auto &a : arrivals) {
            labelW = std::max(labelW, widthOf(mapFont, a.first));
        }
        int labelH = (1 + (int)arrivals.size()) * mapFontHeight + (showName ? smallH : 0);

        // Try the kerb side first: buses drive on the left, so the stop is on
        // the left of the way its arrow points, and the stop across the road
        // labels its own side. Then the other sides, before giving up.
        int gap = (stop.station ? 4 * half : 2 * half) + mapFontWidth / 2;
        float lx = 1, ly = 0;  // default: to the right
        if(heading >= 0 && !stop.station) {
            float hx = sin(heading * M_PI / 180.0f);
            float hy = -cos(heading * M_PI / 180.0f);
            lx = hy;
            ly = -hx;
        }
        enum Side { RIGHT, LEFT, ABOVE, BELOW };
        Side first = fabsf(lx) >= fabsf(ly) ? (lx < 0 ? LEFT : RIGHT) : (ly < 0 ? ABOVE : BELOW);
        Side opposite = first == LEFT ? RIGHT : first == RIGHT ? LEFT : first == ABOVE ? BELOW : ABOVE;
        std::vector<Side> sides = {first, opposite};
        for(Side sd : {RIGHT, LEFT, ABOVE, BELOW}) {
            if(sd != first && sd != opposite) {
                sides.push_back(sd);
            }
        }
        auto boxFor = [&](Side sd) {
            switch(sd) {
                case LEFT:  return SDL_Rect{x - gap - labelW, y - mapFontHeight / 2, labelW, labelH};
                case ABOVE: return SDL_Rect{x - labelW / 2, y - gap - labelH, labelW, labelH};
                case BELOW: return SDL_Rect{x - labelW / 2, y + gap, labelW, labelH};
                default:    return SDL_Rect{x + gap, y - mapFontHeight / 2, labelW, labelH};
            }
        };
        auto clear = [&](const SDL_Rect &b) {
            if(b.x < 0 || b.y < 0 || b.x + b.w > screen_width || b.y + b.h > screen_height) {
                return false;
            }
            for(const SDL_Rect &other : placed) {
                if(SDL_HasIntersection(&b, &other)) {
                    return false;
                }
            }
            for(const SDL_Rect &m : markerRects) {
                if(!(m.x <= x && x <= m.x + m.w && m.y <= y && y <= m.y + m.h) && SDL_HasIntersection(&b, &m)) {
                    return false;  // someone else's marker
                }
            }
            return true;
        };
        SDL_Rect box = boxFor(first);
        Side side = first;
        bool found = false;
        for(Side sd : sides) {
            if(clear(boxFor(sd))) {
                box = boxFor(sd);
                side = sd;
                found = true;
                break;
            }
        }
        if(!found && !selected) {
            continue;
        }
        placed.push_back(box);

        // on the left, lines end at the stop; elsewhere they start from it
        int ty = box.y;
        auto put = [&](TTF_Font *f, const std::string &str, SDL_Color c, int lineH) {
            int lx0 = side == LEFT ? box.x + box.w - widthOf(f, str) : box.x;
            text.setFont(f);
            text.setColor(c);
            text.setPosition(lx0, ty);
            text.setText(str);
            text.draw(renderer);
            ty += lineH;
        };
        put(mapBoldFont, id, fade(style.labelColor), mapFontHeight);
        if(showName) {
            put(stopNameFont, stop.name, fade(style.grey), smallH);  // full name in small type
        }
        for(const auto &a : arrivals) {
            put(mapFont, a.first, a.second, mapFontHeight);
        }
    }

    // home
    float dx, dy;
    int x, y;
    pxFromLonLat(&dx, &dy, transit->lon, transit->lat);
    screenCoords(&x, &y, dx, dy);
    if(!outOfBounds(x, y)) {
        int r = 3 * screen_uiscale;
        circleRGBA(renderer, x, y, r, style.white.r, style.white.g, style.white.b, SDL_ALPHA_OPAQUE);
        lineRGBA(renderer, x - 2 * r, y, x + 2 * r, y, style.white.r, style.white.g, style.white.b, SDL_ALPHA_OPAQUE);
        lineRGBA(renderer, x, y - 2 * r, x, y + 2 * r, style.white.r, style.white.g, style.white.b, SDL_ALPHA_OPAQUE);
        if(labels) {
            text.setColor(style.white);
            text.setPosition(x + 2 * r + mapFontWidth, y - mapFontHeight / 2);
            text.setText("home");
            text.draw(renderer);
        }
    }
}

View::View(AppData *appData){
    this->appData = appData;

    startupState = 0;

    // Display options
    screen_uiscale          = 1;
    pixel_ratio             = 1;

    page                    = PAGE_MAP;
    transit                 = NULL;
    browse                  = NULL;
    browseLat = browseLon   = 0;
    browsePrevLat = browsePrevLon = 0;
    selectedStopHome        = false;
    weather                 = NULL;
    radar                   = NULL;
    showRadar               = false;  // R shows rain over the map
    weatherBigFont          = NULL;
    weatherHugeFont         = NULL;
    weatherBodyFont         = NULL;
    weatherTitleFont        = NULL;
    tickerTexture           = NULL;
    backdropTexture[0] = backdropTexture[1] = NULL;
    backdropLoaded[0] = backdropLoaded[1] = -1;
    tickerW                 = 0;
    tickerH                 = 0;
    weatherSegment          = 0;
    weatherSegmentShown     = std::chrono::steady_clock::now();
    weatherHeadFont         = NULL;
    weatherTabFont          = NULL;
    weatherTextFont         = NULL;
    weatherSmallFont        = NULL;
    rotateSeconds           = 0;
    usePageTimes            = false;
    for(int i = 0; i < PAGE_COUNT; i++) {
        pageSeconds[i] = 0;
    }
    planeFeed               = false;
    flying                  = false;
    transition              = TR_NONE;
    planeFade               = 1;
    homeFitted              = false;
    flySeconds              = 1;
    homeLat = homeLon = 0;
    homeDist = 1;
    planesLat               = 51.505f;  // central London, wide enough for Heathrow to the Thames estuary
    planesLon               = -0.15f;
    planesDist              = 32.0f;
    transitScale            = 1.0;
    uiScaleFactor           = 1.0;
    uiUnit                  = 16;
    transitFont             = NULL;
    transitHeadFont         = NULL;
    miniBoard               = true;
    stopSelected            = false;
    busSelected             = false;
    fadeKind                = FADE_NONE;
    selectionFade           = 0;
    transitStale            = 0;
    lastAnimTime            = std::chrono::steady_clock::now();
    hideCursor              = false;
    noAdsb                  = false;
    miniBoardFont           = NULL;
    stopNameFont            = NULL;
    stopNameFontHeight      = 0;
    transitClockFont        = NULL;
    transitFontWidth        = 0;
    transitFontHeight       = 0;
    pageShownTime           = std::chrono::steady_clock::now();
    screen_width            = 0;
    screen_height           = 0;    
    screen_depth            = 32;
    metric                  = 0;
    fps                     = 0;
    fullscreen              = 0;
    screen_index              = 0;

    highFramerate = false;
    lastFrameTime = 0;

    centerLon   = 0;
    centerLat   = 0;

    maxDist                 = 25.0;

    mapMoved         = 1;
    mapRedraw        = 1;

    selectedAircraft =  NULL;

    std::thread t1(&Map::load, &map);
    t1.detach();
}

View::~View() {
    closeFont(mapFont);
    closeFont(mapBoldFont);
    closeFont(messageFont);
    closeFont(labelFont);
    closeFont(listFont);

    TTF_Quit();
    SDL_Quit();
}
