// RadarData: latest precipitation radar from RainViewer (rainviewer.com, free
// API, attribution required) as a grid of map tiles around a point.
//
// A background thread downloads PNG tiles; the view turns them into textures
// on the render thread (SDL textures can't be made off-thread).

#ifndef RADARDATA_H
#define RADARDATA_H

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RadarTile {
    int z, x, y;                    // slippy-map tile coordinates
    double latTop, latBottom;       // tile bounds
    double lonLeft, lonRight;
    std::vector<unsigned char> png; // encoded image
};

struct RadarFrame {
    int version = 0;                // bumps when new tiles arrive
    time_t time = 0;                // radar timestamp
    std::vector<RadarTile> tiles;
};

class RadarData {
public:
    RadarData();
    ~RadarData();

    void start(double lat, double lon);
    RadarFrame frame();             // copy of the latest frame
    int version();                  // cheap check for new data

private:
    void run();
    bool fetch();

    double lat;
    double lon;
    std::thread worker;
    std::atomic<bool> running;
    std::mutex lock;
    RadarFrame current;
    std::string lastPath;
};

#endif
