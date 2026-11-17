// OsmTiles: street-level map detail (roads, rail, water) from OpenStreetMap,
// fetched for whatever part of the map is on screen.
//
// The map comes as standard zoom-13 vector tiles (~3 km across in London)
// from OpenFreeMap. The view asks for the tiles it can see (want); a
// background thread loads each one from the disk cache (~/.cache/viz1090/osm)
// or, the first time, from the CDN (a fraction of a second each), and
// the main loop picks finished tiles up with poll(). Only a few dozen tiles
// stay in memory; the farthest ones are dropped as the map moves.
//
// Map data (c) OpenMapTiles, (c) OpenStreetMap contributors (ODbL), via OpenFreeMap.

#ifndef OSMTILES_H
#define OSMTILES_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

enum OsmLayer {
    OSM_WATER,
    OSM_ROADS_MINOR,
    OSM_ROADS_MAJOR,
    OSM_RAIL,
    OSM_LAYER_COUNT
};

struct OsmTile {
    float latMin, latMax, lonMin, lonMax;
    // per layer: lon,lat pairs, polylines separated by a NaN pair
    std::vector<float> pts[OSM_LAYER_COUNT];
};

class OsmTiles {
public:
    OsmTiles();
    ~OsmTiles();

    // the area on screen; missing tiles are queued, nearest the centre first
    void want(float latMin, float latMax, float lonMin, float lonMax);

    // move finished tiles in; true if any arrived (the map needs a redraw)
    bool poll();

    // loaded tiles overlapping the area
    std::vector<const OsmTile *> visible(float latMin, float latMax, float lonMin, float lonMax) const;

private:
    typedef int64_t Key;
    static Key keyFor(int ty, int tx);
    static void tileBounds(Key k, OsmTile *t);

    void run();
    bool load(Key k, OsmTile *t);
    bool readCache(const std::string &path, OsmTile *t);
    void writeCache(const std::string &path, const OsmTile &t);
    bool fetch(Key k, OsmTile *t);
    std::string urlTemplate;            // worker thread only
    std::string cachePath(Key k);

    std::map<Key, OsmTile *> tiles;     // main thread only
    float centerLat, centerLon;

    std::mutex lock;
    std::condition_variable wake;
    std::vector<Key> queue;             // wanted, nearest first
    std::set<Key> pending;              // queued or loading
    std::map<Key, long long> failedAt;  // ms; retried after a while
    std::vector<std::pair<Key, OsmTile *>> done;

    std::thread worker;
    std::atomic<bool> running;
};

#endif
