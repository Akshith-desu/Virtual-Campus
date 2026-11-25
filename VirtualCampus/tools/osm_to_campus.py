#!/usr/bin/env python3
"""
osm_to_campus.py - converts an OpenStreetMap export (map.osm) into campus.json
for the Virtual Campus OpenGL app.  Uses only the Python standard library.

    python tools/osm_to_campus.py map.osm assets/campus/campus.json --campus "R. V. College of Engineering"

What it does
  * projects latitude/longitude to local metres around the campus centre
    (x = east, z = south, so it matches OpenGL's right-handed axes with -z = north)
  * buildings: closed ways + multipolygon relations (courtyards become holes)
  * heights: `height` tag, else `building:levels` x 3.5 m, else a default per building type
  * roads/footpaths with a width per highway class; `vehicle` marks roads cars may use
  * green areas, water and parking lots
  * the campus boundary polygon (used to tell on-campus from off-campus buildings)
"""
import argparse
import json
import math
import xml.etree.ElementTree as ET

LEVEL_HEIGHT = 3.5

ROAD_WIDTH = {   # metres
    "trunk": 14, "primary": 11, "secondary": 9, "tertiary": 8, "unclassified": 6,
    "residential": 6, "living_street": 5, "service": 5, "trunk_link": 8, "primary_link": 7,
    "pedestrian": 4, "footway": 2.2, "path": 1.8, "steps": 2.2, "cycleway": 2, "track": 3,
}
VEHICLE_ROADS = {"trunk", "primary", "secondary", "tertiary", "unclassified", "residential",
                 "living_street", "service", "trunk_link", "primary_link"}

DEFAULT_HEIGHT = {"university": 12, "college": 12, "school": 10, "dormitory": 12, "hostel": 12,
                  "apartments": 15, "roof": 4, "garage": 4, "industrial": 6, "retail": 5,
                  "construction": 10, "yes": 8}

AREA_TYPES = {
    ("leisure", "garden"): "green", ("leisure", "park"): "green", ("landuse", "grass"): "green",
    ("leisure", "pitch"): "pitch", ("natural", "scrub"): "scrub", ("natural", "water"): "water",
    ("amenity", "parking"): "parking", ("amenity", "motorcycle_parking"): "parking",
}


# Spelling fixes and short labels (as on the RVCE venue map). Matching is case-insensitive.
NAME_FIXES = {"thoda aur": "", "diamond jubli hostel": "Diamond Jubilee Hostel",
              "bioteach quadrangle": "BT Quadrangle", "dtl huddle": "DTH"}
SHORT_LABELS = {
    "electronics and communication": "ECE Department",
    "telecommunication department": "ETE Department",
    "computer science department": "CSE Department",
    "civil department": "Civil Department",
    "electrical and electronics department": "EEE Department",
    "chemical engineering, chemistry and physics": "Chemical Department",
    "industrial engineering and management department": "IEM Department",
    "aerospace and information science": "ASE / ISE Department",
    "mechanical engineering": "Mechanical Department",
    "biotechnology": "BT Department",
    "administration building & placement department": "Admin & Placement",
    "inter disciplinary research centre (idrc)": "IDRC",
    "kriya kalpa": "Kriyakalpa",
    "new library": "New Library",
}


def tags_of(el):
    return {t.get("k"): t.get("v") for t in el.findall("tag")}


def parse_float(s):
    try:
        return float(str(s).replace("m", "").strip())
    except (TypeError, ValueError):
        return None


def stitch_rings(ways):
    """Join way pieces (lists of node ids) end-to-end into closed rings."""
    pieces = [list(w) for w in ways if len(w) >= 2]
    rings = []
    while pieces:
        ring = pieces.pop(0)
        changed = True
        while ring[0] != ring[-1] and changed:
            changed = False
            for i, p in enumerate(pieces):
                if p[0] == ring[-1]:   ring += p[1:]
                elif p[-1] == ring[-1]: ring += p[::-1][1:]
                elif p[-1] == ring[0]:  ring = p[:-1] + ring
                elif p[0] == ring[0]:   ring = p[::-1][:-1] + ring
                else: continue
                pieces.pop(i); changed = True
                break
        if ring[0] == ring[-1] and len(ring) >= 4:
            rings.append(ring)
    return rings


def point_in_poly(x, z, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, zi = poly[i]; xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            inside = not inside
        j = i
    return inside


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("osm")
    ap.add_argument("out")
    ap.add_argument("--campus", default="", help="name of the campus polygon in OSM (amenity=college/university)")
    args = ap.parse_args()

    root = ET.parse(args.osm).getroot()
    nodes = {n.get("id"): (float(n.get("lat")), float(n.get("lon"))) for n in root.iter("node")}
    ways = {w.get("id"): ([nd.get("ref") for nd in w.findall("nd")], tags_of(w)) for w in root.iter("way")}

    # ---- find the campus boundary
    campus_ring = None
    for wid, (refs, t) in ways.items():
        if t.get("amenity") in ("college", "university") and refs[0] == refs[-1]:
            if not args.campus or t.get("name", "").lower() == args.campus.lower():
                campus_ring = refs
                campus_name = t.get("name", "Campus")
                break
    b = root.find("bounds")
    if campus_ring:
        lats = [nodes[r][0] for r in campus_ring if r in nodes]
        lons = [nodes[r][1] for r in campus_ring if r in nodes]
    else:
        campus_name = args.campus or "Campus"
        lats = [float(b.get("minlat")), float(b.get("maxlat"))]
        lons = [float(b.get("minlon")), float(b.get("maxlon"))]
    lat0 = (min(lats) + max(lats)) / 2
    lon0 = (min(lons) + max(lons)) / 2

    # ---- equirectangular projection around the centre (accurate to cm at campus scale)
    m_per_lat = 110540.0
    m_per_lon = 111320.0 * math.cos(math.radians(lat0))

    def proj(ref):
        lat, lon = nodes[ref]
        return [round((lon - lon0) * m_per_lon, 2), round(-(lat - lat0) * m_per_lat, 2)]

    def ring_xy(refs):
        pts = [proj(r) for r in refs if r in nodes]
        if len(pts) > 1 and pts[0] == pts[-1]:
            pts.pop()
        return pts

    # export window in local metres (roads are clipped to it, plus a margin)
    bminx = (float(b.get("minlon")) - lon0) * m_per_lon - 40
    bmaxx = (float(b.get("maxlon")) - lon0) * m_per_lon + 40
    bminz = -(float(b.get("maxlat")) - lat0) * m_per_lat - 40
    bmaxz = -(float(b.get("minlat")) - lat0) * m_per_lat + 40

    boundary = ring_xy(campus_ring) if campus_ring else []

    def make_building(t, outer, holes):
        btype = t.get("building", "yes")
        h = parse_float(t.get("height"))
        if h is None:
            lv = parse_float(t.get("building:levels"))
            h = lv * LEVEL_HEIGHT + 1.0 if lv and lv > 0 else DEFAULT_HEIGHT.get(btype, 8)
        name = t.get("name", "").strip()
        name = NAME_FIXES.get(name.lower(), name)
        short = SHORT_LABELS.get(name.lower(), name if len(name) <= 24 else name[:22].rstrip() + "...")
        if "hostel" in name.lower():
            btype = "hostel"
        cx = sum(p[0] for p in outer) / len(outer)
        cz = sum(p[1] for p in outer) / len(outer)
        return {"name": name, "short": short, "type": btype, "height": round(h, 1),
                "inCampus": bool(boundary) and point_in_poly(cx, cz, boundary),
                "outer": outer, "holes": holes}

    buildings, roads, areas = [], [], []
    used_as_member = set()

    # ---- multipolygon relations (buildings with courtyards, split outlines)
    for rel in root.iter("relation"):
        t = tags_of(rel)
        if t.get("type") != "multipolygon":
            continue
        outer_w = [ways[m.get("ref")][0] for m in rel.findall("member")
                   if m.get("type") == "way" and m.get("role") == "outer" and m.get("ref") in ways]
        inner_w = [ways[m.get("ref")][0] for m in rel.findall("member")
                   if m.get("type") == "way" and m.get("role") == "inner" and m.get("ref") in ways]
        outers = [ring_xy(r) for r in stitch_rings(outer_w)]
        inners = [ring_xy(r) for r in stitch_rings(inner_w)]
        outers = [o for o in outers if len(o) >= 3]
        if not outers:
            continue
        if "building" in t:
            for m in rel.findall("member"):
                used_as_member.add(m.get("ref"))
            for o in outers:
                hs = [h for h in inners if len(h) >= 3 and point_in_poly(h[0][0], h[0][1], o)]
                buildings.append(make_building(t, o, hs))
        else:
            for (k, v), kind in AREA_TYPES.items():
                if t.get(k) == v:
                    for o in outers:
                        areas.append({"type": kind, "outer": o})

    # ---- plain ways
    for wid, (refs, t) in ways.items():
        closed = len(refs) >= 4 and refs[0] == refs[-1]
        if "building" in t and closed and wid not in used_as_member:
            outer = ring_xy(refs)
            if len(outer) >= 3:
                buildings.append(make_building(t, outer, []))
        elif "highway" in t and t["highway"] in ROAD_WIDTH:
            pts = [proj(r) for r in refs if r in nodes]
            pts = [p for p in pts if bminx <= p[0] <= bmaxx and bminz <= p[1] <= bmaxz]
            if len(pts) >= 2:
                hw = t["highway"]
                roads.append({"name": t.get("name", ""), "type": hw,
                              "width": ROAD_WIDTH[hw], "vehicle": hw in VEHICLE_ROADS,
                              "oneway": t.get("oneway") == "yes", "path": pts})
        elif closed:
            for (k, v), kind in AREA_TYPES.items():
                if t.get(k) == v:
                    outer = ring_xy(refs)
                    if len(outer) >= 3:
                        areas.append({"type": kind, "outer": outer})

    # ---- drop features that reach far outside the exported window
    #      (e.g. a river multipolygon that runs for kilometres)
    def inside_window(poly, margin=60):
        return all(bminx - margin <= x <= bmaxx + margin and bminz - margin <= z <= bmaxz + margin
                   for x, z in poly)
    areas = [a for a in areas if inside_window(a["outer"])]
    buildings = [bd for bd in buildings if inside_window(bd["outer"])]

    out = {
        "name": campus_name,
        "origin": {"lat": lat0, "lon": lon0},
        "bounds": [round(bminx + 40, 1), round(bminz + 40, 1), round(bmaxx - 40, 1), round(bmaxz - 40, 1)],
        "boundary": boundary,
        "buildings": buildings,
        "roads": roads,
        "areas": areas,
        "attribution": "Map data (c) OpenStreetMap contributors, ODbL",
    }
    with open(args.out, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    named = sum(1 for x in buildings if x["name"])
    print(f"{campus_name}: {len(buildings)} buildings ({named} named, "
          f"{sum(x['inCampus'] for x in buildings)} on campus), {len(roads)} roads/paths, "
          f"{len(areas)} areas -> {args.out}")


if __name__ == "__main__":
    main()
