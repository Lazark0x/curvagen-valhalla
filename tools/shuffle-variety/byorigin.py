"""PROTOTYPE per-origin read (research/shuffle-variety): repeats, rescue+fallback share, p90 distance error. usage: byorigin.py run...  (run dirs under ./runs)"""
import json, sys, collections
import variety as V
for d in sys.argv[1:]:
    rows=collections.defaultdict(lambda: [0,0,0,0,0,[]])
    for line in open(f'runs/{d}/cells.jsonl'):
        r=json.loads(line); seen=[]; o=r['origin']
        for s in r['serves']:
            for L in s['loops']:
                pts=V.decode(L['legs'][0])+V.decode(L['legs'][1])[1:]; g=(pts,V.grid(pts))
                if s['n']>0:
                    rows[o][1]+=1; rows[o][0]+= any(V.sim(g,p)>=0.9 for p in seen)
                p=L['provenance']; rows[o][2]+= p.get('rung',0)==2 or p.get('builder')=='rescue'
                rows[o][3]+=1; rows[o][5].append(abs(L['length_km']-r['distance_km'])/r['distance_km'])
            for L in s['loops']:
                pts=V.decode(L['legs'][0])+V.decode(L['legs'][1])[1:]; seen.append((pts,V.grid(pts)))
            rows[o][4]+= len(s['loops'])<3
    print(d, '  '.join(f"{o}: rep {100*v[0]/v[1]:.0f}% fb+rescue {100*v[2]/v[3]:.0f}% p90err {sorted(v[5])[int(.9*len(v[5]))]:.2f} short {v[4]}" for o,v in rows.items()))
