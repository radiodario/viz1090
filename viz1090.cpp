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

#include "AppData.h"
#include "View.h"
#include "Input.h"
#include <cstring> 
int go = 1;


AppData appData;
Style style;

//
// ================================ Main ====================================
//
void showHelp(void) {
    printf(
"-----------------------------------------------------------------------------\n"
"|                        viz1090 ADSB Viewer        Ver : 0.1 |\n"
"-----------------------------------------------------------------------------\n"
"--fps                            Show current framerate\n"
"--fullscreen                     Start fullscreen\n"
"--help                           Show this help\n"
"--lat <latitude>                 Latitude in degrees\n"
"--lon <longitude>                Longitude in degrees\n"
"--metric                         Use metric units\n"
"--port <port>                    TCP Beast output listen port (default: 30005)\n"
"--server <IPv4/hosname>          TCP Beast output listen IPv4 (default: 127.0.0.1)\n"
"--screensize <width> <height>    Set frame buffer resolution (default: screen resolution)\n"
"--screenindex <i>                Set the index of the display to use (default: 0)\n"
"--uiscale <factor>               Scale everything (default 1; sizes follow the screen height)\n"
"--hide-cursor                    Hide the mouse pointer (kiosk screens)\n"
"--no-adsb                        No aircraft: don't connect to dump1090, hide its status\n"
"--zoom <km>                      Initial map radius in km (default: 25)\n"
"--weather                        Add a local weather page (Open-Meteo; W to show) and rain radar on the map (R)\n"
"--weather-start                  Start on the weather page\n"
"--transit                        Add a TfL departures page (toggle with T or Tab)\n"
"--transit-loc <lat> <lon>        Location for departures (default: --lat/--lon)\n"
"--transit-radius <meters>        Walking radius for stops (default: 1000, ~15 min)\n"
"--transit-scale <factor>         Departures text size multiplier (default: 1)\n"
"--transit-start                  Start on the departures page\n"
"--rotate <seconds>               Rotate pages, the same time each\n"
"--page-times <map,board,weather> Rotate pages with their own times; weather 'cycle' = one full pass\n"
    );
}


//
//=========================================================================
//


int main(int argc, char **argv) {
  
    AppData appData;
    View view(&appData);

    TransitBoard transit;
    WeatherData weatherData;
    RadarData radarData;
    bool weatherEnabled = false;
    bool weatherStart = false;
    bool transitEnabled = false;
    bool transitStart = false;
    double transitLat = 0, transitLon = 0;
    int transitRadius = 1000;

    // Parse the command line options
    for (int j = 1; j < argc; j++) {
        int more = ((j + 1) < argc); // There are more arguments

        if        (!strcmp(argv[j],"--port") && more) {
            appData.modes.net_input_beast_port = atoi(argv[++j]);
        } else if (!strcmp(argv[j],"--server") && more) {
            std::strcpy(appData.server, argv[++j]);
        } else if (!strcmp(argv[j],"--lat") && more) {
            appData.modes.fUserLat = atof(argv[++j]);
            view.centerLat = appData.modes.fUserLat;
        } else if (!strcmp(argv[j],"--lon") && more) {
            appData.modes.fUserLon = atof(argv[++j]);
            view.centerLon = appData.modes.fUserLon;
        } else if (!strcmp(argv[j],"--metric")) {
            view.metric = 1;
        } else if (!strcmp(argv[j],"--fps")) {
            view.fps = 1;
        } else if (!strcmp(argv[j],"--fullscreen")) {
            view.fullscreen = 1;
        } else if (!strcmp(argv[j],"--screenindex")) {
            view.screen_index = atoi(argv[++j]);
        } else if (!strcmp(argv[j],"--uiscale") && more) {
            view.uiScaleFactor = atof(argv[++j]);
         } else if (!strcmp(argv[j],"--screensize") && more) {
            view.screen_width = atoi(argv[++j]);
            view.screen_height = atoi(argv[++j]);
        } else if (!strcmp(argv[j],"--no-adsb")) {
            view.noAdsb = true;
        } else if (!strcmp(argv[j],"--hide-cursor")) {
            view.hideCursor = true;
        } else if (!strcmp(argv[j],"--zoom") && more) {
            view.maxDist = atof(argv[++j]);
        } else if (!strcmp(argv[j],"--weather")) {
            weatherEnabled = true;
        } else if (!strcmp(argv[j],"--weather-start")) {
            weatherEnabled = true;
            weatherStart = true;
        } else if (!strcmp(argv[j],"--transit")) {
            transitEnabled = true;
        } else if (!strcmp(argv[j],"--transit-loc") && j + 2 < argc) {
            transitEnabled = true;
            transitLat = atof(argv[++j]);
            transitLon = atof(argv[++j]);
        } else if (!strcmp(argv[j],"--transit-radius") && more) {
            transitEnabled = true;
            transitRadius = atoi(argv[++j]);
        } else if (!strcmp(argv[j],"--transit-scale") && more) {
            transitEnabled = true;
            view.transitScale = atof(argv[++j]);
        } else if (!strcmp(argv[j],"--transit-start")) {
            transitEnabled = true;
            transitStart = true;
        } else if (!strcmp(argv[j],"--page-times") && more) {
            // e.g. 60,30,cycle: map 60 s, departures 30 s, one full weather cycle
            transitEnabled = true;
            weatherEnabled = true;
            view.usePageTimes = true;
            char spec[64];
            strncpy(spec, argv[++j], sizeof(spec) - 1);
            spec[sizeof(spec) - 1] = 0;
            int i = 0;
            for(char *tok = strtok(spec, ","); tok && i < 3; tok = strtok(NULL, ","), i++) {
                view.pageSeconds[i] = strcmp(tok, "cycle") == 0 ? 0 : atof(tok);
            }
        } else if (!strcmp(argv[j],"--rotate") && more) {
            transitEnabled = true;
            view.rotateSeconds = atof(argv[++j]);
        } else if (!strcmp(argv[j],"--help")) {
            showHelp();
            exit(0);
        } else {
            fprintf(stderr, "Unknown or not enough arguments for option '%s'.\n\n", argv[j]);
            showHelp();
            exit(1);
        }
    }


    appData.initialize();

    view.SDL_init();
    view.font_init();

    if(weatherEnabled) {
        if(appData.modes.fUserLat == 0 && appData.modes.fUserLon == 0) {
            fprintf(stderr, "--weather needs --lat/--lon\n");
            exit(1);
        }
        weatherData.start(appData.modes.fUserLat, appData.modes.fUserLon);
        view.weather = &weatherData;
        radarData.start();
        view.radar = &radarData;
        if(weatherStart) {
            view.showPage(View::PAGE_WEATHER);
        }
    }

    if(transitEnabled) {
        if(transitLat == 0 && transitLon == 0) {
            transitLat = appData.modes.fUserLat;
            transitLon = appData.modes.fUserLon;
        }
        if(transitLat == 0 && transitLon == 0) {
            fprintf(stderr, "--transit needs a location: use --transit-loc or --lat/--lon\n");
            exit(1);
        }
        transit.start(transitLat, transitLon, transitRadius);
        view.transit = &transit;
        if(transitStart) {
            view.showPage(View::PAGE_TRANSIT);
        }
    }

    Input input(&appData,&view);

    view.weatherSegmentShown = std::chrono::steady_clock::now();  // rotation starts with the first frame

    signal(SIGINT, SIG_DFL);  // reset signal handler - bit extra safety
    signal(SIGUSR1, [](int) { viewSnapshotRequested = 1; });  // pkill -USR1 viz1090 saves a frame
    signal(SIGUSR2, [](int) { viewNextSegmentRequested = 1; });  // pkill -USR2 viz1090: next weather segment

    int go;
 
    go = 1;
          
    while (go == 1)
    {
        input.getInput();
        view.draw();
        if(!view.noAdsb) {
            appData.connect();
            appData.update();
        }
    }
    
    appData.disconnect();

    return (0);
}
//
//=========================================================================
//
