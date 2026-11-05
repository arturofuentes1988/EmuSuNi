"""The website's world map (site/world.json): each country's outline as an SVG path, keyed by its ISO 3166-1 alpha-2
code, projected once here (Natural Earth I) so the page draws it with no library. The outlines are Natural Earth's
(public domain), from the world-atlas package (countries-110m.json, ISC); the numeric-to-alpha-2 codes from the
world-countries package's list (ODbL: the codes only). Run it again only to change the map.
    python3 -I scripts/make-world-map.py site/world.json"""
import json
import math
import sys
import urllib.request

ATLAS = "https://cdn.jsdelivr.net/npm/world-atlas@2.0.2/countries-110m.json"
CODES = "https://cdn.jsdelivr.net/npm/world-countries@5.1.0/countries.json"
W, H = 960.0, 480.0


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def natural_earth(lon, lat):
    l, p = math.radians(lon), math.radians(lat)
    p2 = p * p
    p4 = p2 * p2
    x = l * (0.8707 - 0.131979 * p2 + p4 * (-0.013791 + p4 * (0.003971 * p2 - 0.001529 * p4)))
    y = p * (1.007226 + p2 * (0.015085 + p4 * (-0.044475 + 0.028874 * p2 - 0.005916 * p4)))
    # (x within about +-2.74, y within about +-1.42 for the whole world)
    return W / 2 + x * (W / 2) / 2.75, H / 2 - y * (H / 2) / 1.45


def main(out):
    topo = get(ATLAS)
    codes = {c.get("ccn3"): c.get("cca2") for c in get(CODES) if c.get("ccn3") and c.get("cca2")}
    sx, sy = topo["transform"]["scale"]
    tx, ty = topo["transform"]["translate"]
    arcs = []
    for arc in topo["arcs"]:
        x = y = 0
        points = []
        for dx, dy in arc:
            x += dx
            y += dy
            points.append((x * sx + tx, y * sy + ty))
        arcs.append(points)

    def ring(indices):
        points = []
        for i in indices:
            a = arcs[i] if i >= 0 else list(reversed(arcs[~i]))
            points.extend(a if not points else a[1:])
        return points

    paths, missing = {}, []
    for g in topo["objects"]["countries"]["geometries"]:
        name = g.get("properties", {}).get("name", "")
        cc = codes.get(str(g.get("id", "")).zfill(3)) if g.get("id") is not None else None
        cc = cc or {"Kosovo": "XK", "Somaliland": "SO", "N. Cyprus": "CY"}.get(name)  # (no numeric code of their own)
        if name == "Antarctica":
            continue
        if not cc:
            missing.append(name)
            continue
        polygons = g["arcs"] if g["type"] == "MultiPolygon" else [g["arcs"]] if g["type"] == "Polygon" else []
        d = []
        for polygon in polygons:
            for r in polygon:
                pts = ring(r)
                last_lon = None
                for k, (lon, lat) in enumerate(pts):
                    x, y = natural_earth(lon, lat)
                    # (A jump across the 180th meridian: a new piece, not a line across the map.)
                    jump = last_lon is not None and abs(lon - last_lon) > 180
                    d.append(("M" if k == 0 or jump else "L") + "%.1f %.1f" % (x, y))
                    last_lon = lon
                d.append("Z")
        paths[cc] = paths.get(cc, "") + "".join(d).replace(" -", "-")
    json.dump({"width": W, "height": H, "source": "Natural Earth (public domain), via world-atlas",
               "paths": paths}, open(out, "w"), separators=(",", ":"))
    print("%d countries in %s%s" % (len(paths), out, (" (no code: %s)" % ", ".join(missing)) if missing else ""))


main(sys.argv[1] if len(sys.argv) > 1 else "site/world.json")
