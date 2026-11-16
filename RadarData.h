// RadarData: precipitation radar from RainViewer (rainviewer.com, free API,
// attribution required): the past ~2 hours in 10-minute frames.
//
// Tiles are loaded on demand, like a slippy map: the view asks for the tiles
// it can see (want()), a background thread downloads them for every frame,
// newest first, and the view turns the PNGs into textures on the render thread.

#ifndef RADARDATA_H
#define RADARDATA_H

#include <atomic>
#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct RadarFrameInfo {
    std::string path;   // RainViewer frame id, part of the tile URL
    time_t time;
};

class RadarData {
public:
    static const int MAX_ZOOM = 7;  // highest zoom RainViewer's free tier serves

    RadarData();
    ~RadarData();

    void start();

    // Frames available, oldest first.
    std::vector<RadarFrameInfo> frames();

    // Ask for tile (z, x, y) in every frame; cheap to call every frame.
    void want(int z, int x, int y);

    // Copy out a downloaded tile's PNG; false if not here yet.
    bool tile(const std::string &path, int z, int x, int y, std::vector<unsigned char> *png);

    // Bumps whenever frames or tiles change.
    int version();

    static std::string key(const std::string &path, int z, int x, int y);

private:
    void run();
    bool refreshFrames();
    bool downloadNext();

    std::thread worker;
    std::atomic<bool> running;
    std::mutex lock;

    std::vector<RadarFrameInfo> frameList;
    std::string host;
    std::map<std::string, std::chrono::steady_clock::time_point> wanted;  // "z/x/y" -> last asked
    std::map<std::string, std::vector<unsigned char>> tiles;             // key() -> png
    std::set<std::string> failed;
    int changes;
};

#endif
