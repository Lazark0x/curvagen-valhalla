#!/usr/bin/env python3
"""PROTOTYPE (wayfinder #50, throwaway) — side-by-side defect-repro viewer.

Regenerates the ticket #47 named worst-case repros from a CANDIDATE engine run
and emits one self-contained HTML file (Leaflet via CDN, data embedded) with
baseline-vs-candidate map panes per case, so Andrey can react to the routes
visually. Baseline loops come from the baseline run's responses (same cells).

Usage: python3 proto_gallery.py <baseline_dir> <candidate_dir> <out_html>
"""

import json
import sys
from pathlib import Path

import metrics


# The #47 gallery cells: (name, response file, slot)
PICKS = [
    ("A golija 300km slot0 — served-first 39.6km stub", "golija_d300_c0.5_s7.json", 0),
    ("B vlasina 200km c0.8 — 88.5km stub, reuse 0.99", "vlasina_d200_c0.8_s11.json", 6),
    ("C zlatibor 200km c0.8 — 44.4km stub", "zlatibor_d200_c0.8_s7.json", 4),
    ("D djerdap 50km c0.8 — worst lollipop stem 0.50", "djerdap_d50_c0.8_s11.json", 11),
    ("E nis 20km — urban lollipop stem 0.41", "nis_d20_c0.5_s7.json", 4),
    ("F vlasina 20km — stem 0.30 reuse 0.83, no spike", "vlasina_d20_c0.5_s101.json", 1),
    ("G novisad 300km — 27km stub in flatland", "novisad_d300_c0.5_s101.json", 4),
    ("H djerdap 100km — textbook seam U-turn 4.3km", "djerdap_d100_c0.5_s7.json", 2),
    ("I vlasina 200km — 26.5km stub + 5 bulbs", "vlasina_d200_c0.5_s11.json", 5),
]

KEEP = ("loop_km", "distance_error", "spike_count", "max_stub_km", "edge_reuse_geom",
        "lollipop_stem_fraction", "bulb_count", "shadow_frac", "compactness",
        "curviness_geom_clean")


def load_case(run_dir, fname, slot):
    fn = Path(run_dir) / "responses" / fname
    if not fn.exists():
        return None
    rec = json.loads(fn.read_text())
    resp, meta = rec["response"], rec["meta"]
    if meta.get("status") != 200:
        return {"error": f"request failed (HTTP {meta.get('status')})", "meta": meta}
    routes = [resp["trip"]] + [a["trip"] for a in resp.get("alternates", [])]
    out = []
    for s in (range(len(routes)) if slot is None else [min(slot, len(routes) - 1)]):
        loop = metrics.Loop.from_engine_trip(routes[s], meta, s)
        r = metrics.analyze_loop(loop)
        coords = [[round(lon, 6), round(lat, 6)]
                  for (lat, lon) in map(metrics.grid_to_ll, loop.pts)]
        seam = metrics.grid_to_ll(loop.pts[loop.seam_index])
        out.append({
            "slot": s,
            "coords": coords,
            "seam": [round(seam[1], 6), round(seam[0], 6)],
            "apexes": [[round(sp["apex_ll"][1], 6), round(sp["apex_ll"][0], 6)]
                       for sp in r["spikes"]],
            "metrics": {k: r[k] for k in KEEP},
        })
    return {"loops": out, "meta": {k: meta[k] for k in
                                   ("origin", "distance_m", "curviness", "seed", "n_routes")}}


def main(base_dir, cand_dir, out_html):
    cases = []
    for name, fname, slot in PICKS:
        cases.append({
            "name": name,
            "file": fname,
            "slot": slot,
            "baseline": load_case(base_dir, fname, slot),
            "candidate": load_case(cand_dir, fname, slot),
        })
    payload = json.dumps(cases)
    html = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>round-trip v3 prototype — repro gallery</title>
<link rel="stylesheet" href="leaflet.css"/>
<script src="leaflet.js"></script>
<style>
body{font-family:-apple-system,sans-serif;margin:0;background:#111;color:#eee}
h1{font-size:16px;padding:10px 16px;margin:0;background:#1a1a1a}
.case{margin:14px 16px;border:1px solid #333;border-radius:8px;overflow:hidden}
.case h2{font-size:14px;margin:0;padding:8px 12px;background:#222}
.panes{display:flex}
.pane{flex:1;min-width:0}
.pane .label{font-size:12px;padding:4px 12px;background:#1c1c1c;color:#aaa}
.map{height:420px}
.m{font-size:11px;padding:6px 12px;color:#9c9;white-space:pre-wrap;font-family:ui-monospace,monospace}
.err{color:#e66;padding:12px}
</style></head><body>
<h1>round-trip v3 prototype (shortlist 1+2+3) — #47 worst-case repros, baseline vs candidate. Same request, same slot rank.</h1>
<div id="cases"></div>
<script>
const CASES = __PAYLOAD__;
function fmt(m){return Object.entries(m).map(([k,v])=>k+": "+v).join("   ")}
function addPane(row, side, data){
  const pane=document.createElement("div");pane.className="pane";
  pane.innerHTML='<div class="label">'+side+'</div>';
  if(!data||data.error){pane.innerHTML+='<div class="err">'+(data?data.error:"missing")+'</div>';row.appendChild(pane);return}
  const mapDiv=document.createElement("div");mapDiv.className="map";pane.appendChild(mapDiv);
  const lp=data.loops[0];
  const met=document.createElement("div");met.className="m";met.textContent=fmt(lp.metrics);pane.appendChild(met);
  row.appendChild(pane);
  const map=L.map(mapDiv,{scrollWheelZoom:false});
  L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19}).addTo(map);
  const line=L.polyline(lp.coords.map(c=>[c[1],c[0]]),{color:side==="baseline (fix44)"?"#e8452d":"#00d4aa",weight:3,opacity:.9}).addTo(map);
  L.circleMarker([lp.seam[1],lp.seam[0]],{radius:6,color:"#ffd54b",fillOpacity:.9}).addTo(map).bindTooltip("turnaround");
  lp.apexes.forEach(a=>L.circleMarker([a[1],a[0]],{radius:4,color:"#f0f",fillOpacity:.9}).addTo(map).bindTooltip("spike apex"));
  setTimeout(()=>{map.invalidateSize();map.fitBounds(line.getBounds().pad(0.08));},50);
}
const root=document.getElementById("cases");
CASES.forEach(c=>{
  const div=document.createElement("div");div.className="case";
  div.innerHTML="<h2>"+c.name+"  <span style='color:#888'>("+c.file+" slot "+c.slot+")</span></h2>";
  const row=document.createElement("div");row.className="panes";
  addPane(row,"baseline (fix44)",c.baseline);
  addPane(row,"candidate (proto-v3)",c.candidate);
  div.appendChild(row);root.appendChild(div);
});
</script></body></html>"""
    Path(out_html).write_text(html.replace("__PAYLOAD__", payload))
    print(f"wrote {out_html} ({len(cases)} cases)")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
