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

#ifndef VIEW_H
#define VIEW_H

#include "AppData.h"
#include "Map.h"
#include "Style.h"
#include "TransitBoard.h"
#include "WeatherData.h"
#include "RadarData.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_ttf.h" 
#include <chrono>
#include <csignal>
#include <map>
#include <string>

extern volatile sig_atomic_t viewSnapshotRequested;
extern volatile sig_atomic_t viewNextSegmentRequested;


//defs - should all move to config file setup
#define ROUND_RADIUS 3 //radius of text box corners

#define TRAIL_LENGTH 120
#define TRAIL_TTL   240.0 
#define DISPLAY_ACTIVE   30.0
#define TRAIL_TTL_STEP   2

#define MIN_MAP_FEATURE 2

#define FRAMETIME 33

#define PAD 5

#define LATLONMULT 111.195 // 6371.0 * M_PI / 180.0


class View {

	private:
		AppData *appData;

		//for cursor drawing
	    std::chrono::high_resolution_clock::time_point mouseMovedTime;
	    bool mouseMoved;
	    int mousex;
	    int mousey;

	    std::chrono::high_resolution_clock::time_point clickTime;
	    bool clicked;
	    int clickx;
	    int clicky;

	    int lineCount;

	    float dx_mult;
	    float dy_mult;

	    TTF_Font* loadFont(const char *name, int size);
	    void closeFont(TTF_Font *font);
		SDL_Rect drawString(std::string text, int x, int y, TTF_Font *font, SDL_Color color);
		SDL_Rect drawStringBG(std::string text, int x, int y, TTF_Font *font, SDL_Color color, SDL_Color bgColor);
		void drawStatusBox(int *left, int *top, std::string label, std::string message, SDL_Color color);
		void drawCenteredStatusBox(std::string label, std::string message, SDL_Color color);
		void drawStatus();
		void moveLabels(float dx, float dy);

		Aircraft *selectedAircraft;

		Style style;

	public:
		int screenDist(float d);
		void pxFromLonLat(float *dx, float *dy, float lon, float lat);
		void latLonFromScreenCoords(float *lat, float *lon, int x, int y);
		void screenCoords(int *outX, int *outY, float dx, float dy);
		int outOfBounds(int x, int y);
		int outOfBounds(int x, int y, int left, int top, int right, int bottom);
		void drawPlaneOffMap(int x, int y, int *returnx, int *returny, SDL_Color planeColor);
		void drawPlaneIcon(int x, int y, float heading, SDL_Color planeColor);
		void drawTrails(int left, int top, int right, int bottom);
		void drawScaleBars();
		void drawLinesRecursive(QuadTree *tree, float screen_lat_min, float screen_lat_max, float screen_lon_min, float screen_lon_max, SDL_Color color);
		void drawLines(int left, int top, int right, int bottom, int bailTime);
		void drawPlaceNames();		
		void drawGeography();
		void drawSignalMarks(Aircraft *p, int x, int y);
		void drawPlaneText(Aircraft *p);
		void resolveLabelConflicts();
		void drawPlanes();
		void animateCenterAbsolute(float x, float y);
		void moveCenterAbsolute(float x, float y);
		void moveCenterRelative(float dx, float dy);
		void zoomMapToTarget();
		void moveMapToTarget();
		void drawMouse();
		void drawClick();
		void registerClick(int tapcount, int x, int y);
		void registerMouseMove(int x, int y);
		void draw();

		// pages: the aircraft map, and the TfL transit board
		enum Page { PAGE_MAP, PAGE_TRANSIT, PAGE_WEATHER };
		Page page;
		TransitBoard *transit;          // NULL unless --transit is given
		WeatherData *weather;           // NULL unless --weather is given
		RadarData *radar;               // rain radar (with --weather)
		bool showRadar;                 // map overlay, toggled with R
		struct RadarTexture {
			SDL_Texture *texture;
			std::chrono::steady_clock::time_point lastUsed;
		};
		std::map<std::string, RadarTexture> radarTextures;  // RadarData::key -> texture
		int radarLoopFrame(int frames);  // which frame of the loop to show now
		void drawRadarTiles(Uint8 alpha, bool animate, time_t *frameTime);  // visible tiles: looping, or the latest frame
		void drawRadar();               // map overlay
		void drawWeather();
		void drawWeatherGlyph(int code, bool isDay, int cx, int cy, int px, float anim = -1);
		void drawWeatherRadar(int top, int bottom);  // radar segment of the weather page
		void drawRadarLegend(int right, int top, int height);
		void drawWeatherRegional(const WeatherSnapshot &w, int top, int bottom, float anim);
		void drawTicker(const std::string &text, int y, float anim);
		SDL_Texture *tickerTexture;
		std::string tickerText;
		int tickerW, tickerH;
		TTF_Font *weatherTitleFont;     // segment titles
		int weatherSegment;              // which rotating segment is showing
		std::chrono::steady_clock::time_point weatherSegmentShown;
		void nextWeatherSegment(int dir);
		TTF_Font *weatherHugeFont;
		TTF_Font *weatherBodyFont;      // larger Terminus for the weather details
		TTF_Font *weatherBigFont;
		TTF_Font *weatherHeadFont;
		TTF_Font *weatherTabFont;
		TTF_Font *weatherTextFont;
		TTF_Font *weatherSmallFont;
		float rotateSeconds;            // 0 = no automatic page rotation
		float transitScale;             // board text size relative to the map UI
		TTF_Font *transitFont;          // London Underground Regular
		TTF_Font *transitHeadFont;      // London Underground Heavy, for the heading
		TTF_Font *transitClockFont;     // London Underground Bold (digits and ':')
		int transitFontWidth;
		int transitFontHeight;
		std::chrono::steady_clock::time_point pageShownTime;
		void presentFrame();            // present, saving a snapshot if requested
		void showPage(Page newPage);
		void togglePage();
		void drawTransit();
		void drawTransitStops();        // stop markers on the map page
		void drawMiniBoard();           // next few departures over the map
		float stopArrowHeading(const StopMarker &stop);  // TfL compass point snapped to the nearest road
		std::map<std::string, float> stopHeadingCache;

		// tapped stop: walking route from home is drawn on the map
		bool stopSelected;
		StopMarker selectedStop;
		void drawWalkRoute();
		void drawBuses();               // estimated bus positions
		void drawBusRoute();            // selected bus: route ahead and next stops
		std::vector<BusMarker> frameBuses;  // bus positions for this frame
		bool busSelected;
		std::string selectedBusId;
		const BusMarker *selectedBus();

		// animation state, in the spirit of the aircraft: buses ease to new
		// estimates and burst in when new; selection fades in and out
		struct BusAnim {
			double lat;
			double lon;
			std::chrono::steady_clock::time_point firstSeen;
			std::chrono::steady_clock::time_point lastSeen;
		};
		std::map<std::string, BusAnim> busAnim;
		enum FadeKind { FADE_NONE, FADE_STOP, FADE_BUS };
		FadeKind fadeKind;              // what the current (or fading-out) selection is
		float selectionFade;            // 0 = nothing dimmed, 1 = fully dimmed
		float transitStale;             // 0 = fresh TfL data, 1 = very stale
		std::chrono::steady_clock::time_point lastAnimTime;
		void updateTransitAnimation();
		bool stopDimmed(const StopMarker &stop);
		bool busDimmed(const BusMarker &bus);
		SDL_Color transitColor(SDL_Color c, bool dimmed);
		bool busCallsAt(const BusMarker &bus, const std::string &stopId);
		bool miniBoard;                 // toggled with B
		TTF_Font *miniBoardFont;
		TTF_Font *stopNameFont;         // small type for stop names on the map
		int stopNameFontHeight;

		void SDL_init();
		void font_init();

		View(AppData *appData);
		~View();


	////////////////
		bool metric;

		bool fps;

	    int startupState;

	    float maxDist;
	    float currentMaxDist;

	    float centerLon;
	    float centerLat;
	   
	    float mapTargetMaxDist;
	    float mapTargetLat;
	    float mapTargetLon;

	    int mapMoved;
	    int mapRedraw;
	    int mapAnimating;

	    bool highFramerate;

	    float currentLon;
	    float currentLat;
	    float lastFrameTime;
	    std::chrono::high_resolution_clock::time_point drawStartTime;
	    std::chrono::high_resolution_clock::time_point lastRedraw;

	    Map map;

	    int screen_upscale;
	    int screen_uiscale;
	    int pixel_ratio;     // drawable pixels per window point (2 on Retina)
	    int screen_width;
	    int screen_height;
	    int screen_depth;
	    int fullscreen;
	    bool hideCursor;
	    bool noAdsb;                // no aircraft source: skip the decoder entirely
	    int screen_index;

		SDL_Window		*window;
		SDL_Renderer	*renderer;
		SDL_Texture 	*mapTexture;

		TTF_Font		*mapFont;
		TTF_Font		*mapBoldFont;	
		TTF_Font		*listFont;	

		TTF_Font		*messageFont;	
		TTF_Font		*labelFont;		

		int mapFontWidth;
		int mapFontHeight;
		int labelFontWidth;
		int labelFontHeight;	
		int messageFontWidth;
		int messageFontHeight;
};

#endif
