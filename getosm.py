#!/usr/bin/env python3
"""Fetch OpenStreetMap roads, railways and water around a point for viz1090.

Writes viz1090's map format (float32 lon,lat pairs, polylines separated by
0,0) into one file per layer:

    roads_major.bin   motorway .. tertiary
    roads_minor.bin   residential, unclassified, living streets
    rail.bin          railways (overground, national rail, tube above ground)
    water.bin         rivers, canals, reservoirs, lakes
    airportdata.bin   runways (wider radius, see --airport-radius)
    airportnames      airport IATA codes, "lon lat CODE" per line

viz1090 now loads street detail itself, as vector tiles for whatever is on
screen (see OsmTiles.cpp), and ignores the four street files: this script is
only needed for airportdata.bin and airportnames.

Uses only the Python standard library, so it runs as-is on a Raspberry Pi.
Map data (c) OpenStreetMap contributors, ODbL.

    python3 getosm.py --lat 51.5074 --lon -0.1278 --radius 4
"""

import argparse
import json
import math
import struct
import sys
import time
import urllib.parse
import urllib.request

# Public Overpass servers, tried in order; the main one is often busy.
OVERPASS_URLS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
]

MAJOR = {"motorway", "trunk", "primary", "secondary", "tertiary",
         "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link"}
MINOR = {"residential", "unclassified", "living_street", "pedestrian"}


def bbox(lat, lon, radius_km):
    dlat = radius_km / 111.195
    dlon = radius_km / (111.195 * math.cos(math.radians(lat)))
    return (lat - dlat, lon - dlon, lat + dlat, lon + dlon)


def query(box):
    s, w, n, e = box
    b = f"{s:.6f},{w:.6f},{n:.6f},{e:.6f}"
    highways = "|".join(sorted(MAJOR | MINOR))
    return f"""
[out:json][timeout:120];
(
  way["highway"~"^({highways})$"]({b});
  way["railway"~"^(rail|light_rail|subway)$"]["tunnel"!="yes"]({b});
  way["waterway"~"^(river|canal)$"]({b});
  way["natural"="water"]({b});
  way["landuse"="reservoir"]({b});
);
out geom;
"""


def airport_query(box):
    s, w, n, e = box
    b = f"{s:.6f},{w:.6f},{n:.6f},{e:.6f}"
    return f"""
[out:json][timeout:120];
way["aeroway"="runway"]({b});
out geom;
nwr["aeroway"="aerodrome"]["iata"]({b});
out center;
"""


def fetch(q):
    data = urllib.parse.urlencode({"data": q}).encode()
    last_error = None
    for attempt in range(2):
        for url in OVERPASS_URLS:
            req = urllib.request.Request(url, data=data,
                                         headers={"User-Agent": "viz1090-getosm/1.0"})
            try:
                with urllib.request.urlopen(req, timeout=180) as resp:
                    return json.load(resp)
            except Exception as e:  # busy server, timeout, bad gateway...
                print(f"  {url}: {e}", file=sys.stderr)
                last_error = e
        time.sleep(10)
    raise SystemExit(f"All Overpass servers failed: {last_error}")


def layer_for(tags):
    hw = tags.get("highway")
    if hw in MAJOR:
        return "roads_major"
    if hw in MINOR:
        return "roads_minor"
    if "railway" in tags:
        return "rail"
    if "waterway" in tags or tags.get("natural") == "water" or tags.get("landuse") == "reservoir":
        return "water"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--radius", type=float, default=4.0, help="half-width of the box in km (default 4)")
    ap.add_argument("--airport-radius", type=float, default=60.0,
                    help="half-width of the box for runways and airport names in km (default 60, 0 to skip)")
    args = ap.parse_args()

    box = bbox(args.lat, args.lon, args.radius)
    print(f"Fetching OSM data for {args.radius} km around {args.lat}, {args.lon} ...")
    result = fetch(query(box))

    layers = {"roads_major": [], "roads_minor": [], "rail": [], "water": []}
    counts = {k: 0 for k in layers}

    for el in result.get("elements", []):
        if el.get("type") != "way" or "geometry" not in el:
            continue
        name = layer_for(el.get("tags", {}))
        if not name:
            continue
        pts = layers[name]
        for p in el["geometry"]:
            pts.extend((p["lon"], p["lat"]))
        pts.extend((0.0, 0.0))  # polyline separator
        counts[name] += 1

    for name, pts in layers.items():
        with open(f"{name}.bin", "wb") as f:
            f.write(struct.pack(f"<{len(pts)}f", *pts))
        print(f"{name}.bin: {counts[name]} ways, {len(pts) // 2} points")

    if not any(counts.values()):
        print("No data returned", file=sys.stderr)
        sys.exit(1)

    if args.airport_radius > 0:
        print(f"Fetching runways and airports for {args.airport_radius} km around ...")
        result = fetch(airport_query(bbox(args.lat, args.lon, args.airport_radius)))
        runway_pts, names, runways = [], [], 0
        for el in result.get("elements", []):
            tags = el.get("tags", {})
            if tags.get("aeroway") == "runway" and "geometry" in el:
                for p in el["geometry"]:
                    runway_pts.extend((p["lon"], p["lat"]))
                runway_pts.extend((0.0, 0.0))
                runways += 1
            elif tags.get("aeroway") == "aerodrome":
                c = el.get("center") or {"lat": el.get("lat"), "lon": el.get("lon")}
                if c.get("lat") is not None:
                    names.append(f"{c['lon']} {c['lat']} {tags['iata']}")
        with open("airportdata.bin", "wb") as f:
            f.write(struct.pack(f"<{len(runway_pts)}f", *runway_pts))
        with open("airportnames", "w") as f:
            f.write("\n".join(names) + ("\n" if names else ""))
        print(f"airportdata.bin: {runways} runways; airportnames: {', '.join(n.split()[-1] for n in names)}")


if __name__ == "__main__":
    main()
