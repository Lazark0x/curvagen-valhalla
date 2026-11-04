"""Edge-curvature census on the rig tiles: the tile's 4-bit curvature (the fork's turning-density
metric) vs upstream's averaged-radius metric recomputed from the same edge shape.
usage: python3 curvhist.py ENGINE_URL OUT.json"""
import json, math, sys, urllib.request, itertools, collections
BASE, OUT = sys.argv[1], sys.argv[2]
R = 6378160.187

def post(path, body):
    req = urllib.request.Request(BASE + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)

def decode(s, prec=6):
    pts = []; i = lat = lon = 0
    while i < len(s):
        for which in (0, 1):
            shift = res = 0
            while True:
                b = ord(s[i]) - 63; i += 1; res |= (b & 0x1f) << shift; shift += 5
                if b < 0x20: break
            d = ~(res >> 1) if res & 1 else res >> 1
            if which == 0: lat += d
            else: lon += d
        pts.append((lat / 10 ** prec, lon / 10 ** prec))
    return pts

def dist(a, b):  # haversine, metres (midgard uses the same radius)
    la1, la2 = math.radians(a[0]), math.radians(b[0])
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin(math.radians(b[1] - a[1]) / 2) ** 2
    return 2 * R * math.asin(math.sqrt(min(1, h)))

def heading(a, b):
    if a == b: return 0.0
    la1, la2 = math.radians(a[0]), math.radians(b[0]); dl = math.radians(b[1] - a[1])
    y = math.sin(dl) * math.cos(la2); x = math.cos(la1) * math.sin(la2) - math.sin(la1) * math.cos(la2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360

def radius(p1, p2, p3):
    a, b, c = dist(p1, p2), dist(p2, p3), dist(p1, p3)
    s = (a + b + c) / 2; k2 = s * (s - a) * (s - b) * (s - c)
    if k2 <= 0: return float("inf")
    return a * b * c / (4 * math.sqrt(k2))

def upstream_curv(sh):  # src/mjolnir/util.cc compute_curvature at upstream master
    if len(sh) == 2: return 0
    tot = n = 0
    for p1, p2, p3 in zip(sh, sh[1:], sh[2:]):
        r = radius(p1, p2, p3)
        if math.isnan(r): continue
        score = 0.0 if r > 1000 else (1500.0 / r if r > 0 else 25.0)
        tot += min(score, 25.0); n += 1
    avg = tot / n if n else 0.0
    return 15 if avg > 15 else int(avg)

def dp(sh, eps):  # Douglas-Peucker, metres, local equirectangular projection
    if len(sh) < 3: return sh
    lat0 = math.radians(sh[0][0])
    xy = [(math.radians(p[1]) * R * math.cos(lat0), math.radians(p[0]) * R) for p in sh]
    keep = [False] * len(sh); keep[0] = keep[-1] = True
    stack = [(0, len(sh) - 1)]
    while stack:
        s, e = stack.pop()
        (x1, y1), (x2, y2) = xy[s], xy[e]
        dx, dy = x2 - x1, y2 - y1; L2 = dx * dx + dy * dy
        best, bi = -1, None
        for i in range(s + 1, e):
            px, py = xy[i]
            t = 0 if L2 == 0 else max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / L2))
            d = (px - x1 - t * dx) ** 2 + (py - y1 - t * dy) ** 2
            if d > best: best, bi = d, i
        if bi is not None and best > eps * eps:
            keep[bi] = True; stack += [(s, bi), (bi, e)]
    return [p for p, k in zip(sh, keep) if k]

def fork_curv(sh):  # the fork's compute_curvature (turning density, 350 deg/km = 15)
    if len(sh) < 3: return 0
    pts = dp(sh, 5.0)
    if len(pts) < 3: return 0
    turn = 0.0
    for i in range(1, len(pts) - 1):
        d = abs(heading(pts[i], pts[i + 1]) - heading(pts[i - 1], pts[i]))
        if d > 180: d = 360 - d
        turn += d
    L = sum(dist(a, b) for a, b in zip(pts, pts[1:]))
    if L < 1: return 0
    b = (turn / L * 1000.0) / 350.0 * 15.0
    return 15 if b >= 15 else int(b + 0.5)

ORIG = {"belgrade-vracar": (44.7979, 20.4722), "novi-sad": (45.2671, 19.8335), "nis": (43.3209, 21.8958),
        "kragujevac": (44.0128, 20.9114), "zlatibor": (43.7290, 19.7000), "subotica": (46.1003, 19.6658)}
TOWNS = {"valjevo": (44.2700, 19.8900), "cacak": (43.8914, 20.3497), "uzice": (43.8586, 19.8488),
         "kraljevo": (43.7258, 20.6894), "pirot": (43.1531, 22.5861), "zajecar": (43.9036, 22.2742),
         "sabac": (44.7550, 19.6900), "smederevo": (44.6650, 20.9300), "novi-pazar": (43.1367, 20.5122),
         "vranje": (42.5514, 21.9000), "sombor": (45.7740, 19.1150), "zrenjanin": (45.3800, 20.3900)}

def shapes():
    out = []
    for o, (la, lo) in ORIG.items():
        for c in (0.5, 1.0):
            cost = {"motorcycle": {"use_highways": round(max(0.1, 1 - c * .7), 2), "use_trails": round(min(c * .8, .8), 2),
                    "use_tolls": .5, "top_speed": int(120 - c * 40), "maneuver_penalty": max(int(10 - c * 10), 0),
                    "prefer_curvature": c, "reuse_penalty": .8, "curviness_continuity": c, "prefer_elevation": .3}}
            body = {"locations": [{"lat": la, "lon": lo}] * 2, "costing": "motorcycle", "costing_options": cost,
                    "roundtrip": {"target_distance": 60000, "num_candidates": 3, "seed": 7},
                    "units": "kilometers", "directions_type": "none"}
            d = post("/route", body)
            trips = [d["trip"]] + [a["trip"] for a in d.get("alternates") or []]
            for t in trips:
                for leg in t["legs"]:
                    out.append(("loop", leg["shape"]))
    towns = list(TOWNS.values()) + list(ORIG.values())
    for a, b in zip(towns, towns[3:] + towns[:3]):
        body = {"locations": [{"lat": a[0], "lon": a[1]}, {"lat": b[0], "lon": b[1]}], "costing": "motorcycle",
                "units": "kilometers", "directions_type": "none"}
        d = post("/route", body)
        out.append(("ab", d["trip"]["legs"][0]["shape"]))
    return out

def main():
    edges = {}
    for kind, sh in shapes():
        body = {"encoded_polyline": sh, "costing": "motorcycle", "shape_match": "edge_walk",
                "filters": {"attributes": ["edge.id", "edge.curvature", "edge.density", "edge.length", "edge.road_class",
                                           "edge.use", "edge.begin_shape_index", "edge.end_shape_index", "edge.roundabout",
                                           "shape"], "action": "include"}}
        try:
            d = post("/trace_attributes", body)
        except Exception as e:
            print("trace fail", kind, e, file=sys.stderr); continue
        pts = decode(d["shape"])
        es = d["edges"]
        for e in es[1:-1]:  # first/last edges are partial
            eid = e.get("id")
            if eid in edges: continue
            sh = pts[e["begin_shape_index"]: e["end_shape_index"] + 1]
            edges[eid] = {"kind": kind, "tile": e.get("curvature", 0), "fork": fork_curv(sh), "up": upstream_curv(sh),
                          "density": e.get("density"), "km": e.get("length"), "rc": e.get("road_class"),
                          "use": e.get("use"), "rb": e.get("roundabout", False), "npts": len(sh)}
    json.dump(list(edges.values()), open(OUT, "w"))
    E = list(edges.values())
    agree = sum(1 for e in E if e["tile"] == e["fork"]); near = sum(1 for e in E if abs(e["tile"] - e["fork"]) <= 1)
    print(f"edges {len(E)}  tile==fork-metric {agree} ({100*agree/len(E):.1f}%)  within 1 {100*near/len(E):.1f}%")

if __name__ == "__main__":
    main()
