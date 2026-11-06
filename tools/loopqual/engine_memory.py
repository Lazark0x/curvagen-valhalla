#!/usr/bin/env python3
"""Engine memory and wall time per Round Trip request, read from the engine
container's own cgroup.

Two steps, because the corpus is YAML and the measuring side has no PyYAML:

  # host (tools/loopqual/venv): corpus -> the exact engine request of every job
  engine_memory.py jobs --corpus corpus-aunz-v1.yaml > jobs.json

  # inside the Docker VM, beside the engine: same clock, same network, the
  # engine's cgroup readable (stdlib only; the fork image's python3 will do)
  docker run --rm --network container:$ENGINE --cgroupns host \\
    -v $PWD:$PWD -w $PWD --entrypoint python3 $IMAGE \\
    engine_memory.py fire --engine-id $(docker inspect -f '{{.Id}}' $ENGINE) \\
    --jobs jobs.json --out results/run --parallel 1

`fire` samples memory.current and memory.stat (anon, file) every 20 ms, fires
the jobs (`--parallel` at a time; `--pause` seconds between sequential
requests), and writes:

  responses/<job>.json  loopqual's own record format, so `loopqual run` on the
                        same --out reads them as its resume cache and adds the
                        loop metrics
  requests.jsonl        per request: start/end, wall, status, served lengths,
                        provenance, peak memory.current / anon during it, the
                        anon high-water mark of the container so far
  samples.csv           every sample (t, current, anon, file)
  phase.json            memory.peak before and after, the phase's peak anon

`startup` samples the engine from container start until /status answers
(the round-trip sidecar build reads every tile): wall and peak memory.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def cgroup_dir(engine_id: str) -> str:
    hits = [d for d in glob.glob(f"/sys/fs/cgroup/**/*{engine_id}*", recursive=True)
            if os.path.isfile(os.path.join(d, "memory.current"))]
    if not hits:
        raise SystemExit(f"no cgroup with memory.current for {engine_id} (run with --cgroupns host)")
    return min(hits, key=len)


class Sampler(threading.Thread):
    def __init__(self, cg: str, period_s: float = 0.02):
        super().__init__(daemon=True)
        self.cg, self.period = cg, period_s
        self.samples = []  # (t, current, anon, file)
        self.lock = threading.Lock()
        self.stop = threading.Event()

    def read(self):
        with open(os.path.join(self.cg, "memory.current")) as f:
            current = int(f.read())
        anon = file = 0
        with open(os.path.join(self.cg, "memory.stat")) as f:
            for line in f:
                k, v = line.split()
                if k == "anon":
                    anon = int(v)
                elif k == "file":
                    file = int(v)
                    break
        return current, anon, file

    def run(self):
        while not self.stop.is_set():
            s = (time.time(), *self.read())
            with self.lock:
                self.samples.append(s)
            time.sleep(self.period)

    def window(self, t0, t1):
        with self.lock:
            w = [s for s in self.samples if t0 <= s[0] <= t1]
        return w

    def high_water_anon(self):
        with self.lock:
            return max((s[2] for s in self.samples), default=0)

    def peak(self):
        with open(os.path.join(self.cg, "memory.peak")) as f:
            return int(f.read())


MiB = 1 << 20


def served(resp):
    trips = [resp["trip"]] + [a["trip"] for a in resp.get("alternates", [])]
    out = []
    for t in trips:
        p = t.get("curvagen_provenance") or t.get("provenance") or {}
        out.append({"km": t["summary"]["length"], **{k: p.get(k) for k in
                    ("builder", "rung", "tier", "relaxed", "gated", "full_repair")}})
    return out


def fire(args):
    import runner  # loopqual's own request builder: the orchestrator's engine request, verbatim

    jobs = json.loads(Path(args.jobs).read_text())
    out = Path(args.out)
    (out / "responses").mkdir(parents=True, exist_ok=True)
    sampler = Sampler(cgroup_dir(args.engine_id))
    peak_before = sampler.peak()
    sampler.start()
    time.sleep(0.5)
    idle = sampler.read()
    log = (out / "requests.jsonl").open("a")
    log_lock = threading.Lock()

    def one(job):
        path, body = runner.build_request(job, serving=False)
        t0 = time.time()
        status, dt, resp = runner.post_json(args.engine.rstrip("/") + path, body,
                                            timeout=args.timeout)
        t1 = time.time()
        w = sampler.window(t0, t1 + 0.05)
        rec = {
            "file": runner.job_filename(job), "origin": job["origin"],
            "distance_m": job["distance_m"], "curviness": job["curviness"], "seed": job["seed"],
            "k": job["k"], "t0": round(t0, 3), "t1": round(t1, 3), "wall_s": round(dt, 3),
            "status": status,
            "peak_current_mib": round(max((s[1] for s in w), default=0) / MiB, 1),
            "peak_anon_mib": round(max((s[2] for s in w), default=0) / MiB, 1),
            "anon_before_mib": round(w[0][2] / MiB, 1) if w else None,
            "anon_high_water_mib": round(sampler.high_water_anon() / MiB, 1),
            "samples": len(w),
        }
        if status == 200:
            rec["served"] = served(resp)
        else:
            rec["error"] = json.dumps(resp)[:300]
        record = {
            "meta": {"origin": job["origin"], "lat": job["lat"], "lon": job["lon"],
                     "distance_m": job["distance_m"], "curviness": job["curviness"],
                     "seed": job["seed"], "k": job["k"], "mode": "engine", "status": status,
                     "latency_s": round(dt, 3),
                     "n_routes": (1 + len(resp.get("alternates", []))) if status == 200 else 0,
                     "engine": args.engine_note or args.engine},
            "request": body,
            "response": resp,
        }
        if args.parallel == 1 or not (out / "responses" / rec["file"]).exists():
            (out / "responses" / rec["file"]).write_text(json.dumps(record))
        with log_lock:
            log.write(json.dumps(rec) + "\n")
            log.flush()
        km = "/".join(f"{s['km']:.0f}" for s in rec.get("served", []))
        print(f"{rec['file']}: {status} {dt:.2f}s [{km}] peak anon {rec['peak_anon_mib']} MiB "
              f"(hw {rec['anon_high_water_mib']})", flush=True)
        return rec

    t_start = time.time()
    if args.parallel == 1:
        for job in jobs:
            one(job)
            time.sleep(args.pause)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel) as ex:
            list(ex.map(one, jobs))
    t_end = time.time()
    time.sleep(0.3)
    sampler.stop.set()
    sampler.join()
    with (out / "samples.csv").open("w") as f:
        f.write("t,current,anon,file\n")
        for s in sampler.samples:
            f.write(f"{s[0]:.3f},{s[1]},{s[2]},{s[3]}\n")
    phase = {
        "parallel": args.parallel, "jobs": len(jobs), "wall_s": round(t_end - t_start, 1),
        "idle_before": {"current_mib": round(idle[0] / MiB, 1), "anon_mib": round(idle[1] / MiB, 1),
                        "file_mib": round(idle[2] / MiB, 1)},
        "phase_peak_current_mib": round(max(s[1] for s in sampler.samples) / MiB, 1),
        "phase_peak_anon_mib": round(max(s[2] for s in sampler.samples) / MiB, 1),
        "memory_peak_before_mib": round(peak_before / MiB, 1),
        "memory_peak_after_mib": round(sampler.peak() / MiB, 1),
    }
    (out / "phase.json").write_text(json.dumps(phase, indent=1) + "\n")
    print(json.dumps(phase), flush=True)


def startup(args):
    sampler = Sampler(cgroup_dir(args.engine_id), period_s=0.1)
    sampler.start()
    t0 = time.time()
    status = None
    while time.time() - t0 < args.timeout:
        try:
            with urllib.request.urlopen(args.engine.rstrip("/") + "/status", timeout=5) as r:
                status = json.load(r)
                break
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(0.5)
    t1 = time.time()
    time.sleep(1.0)
    sampler.stop.set()
    sampler.join()
    s = sampler.samples
    out = {
        "ready_s": round(t1 - t0, 1) if status else None,
        "status": {k: status.get(k) for k in ("version", "engine_curvature_metric",
                                              "tileset_curvature_metric")} if status else None,
        "peak_anon_mib": round(max(x[2] for x in s) / MiB, 1),
        "peak_current_mib": round(max(x[1] for x in s) / MiB, 1),
        "ready": {"current_mib": round(s[-1][1] / MiB, 1), "anon_mib": round(s[-1][2] / MiB, 1),
                  "file_mib": round(s[-1][3] / MiB, 1)},
        "memory_peak_mib": round(sampler.peak() / MiB, 1),
    }
    Path(args.out).mkdir(parents=True, exist_ok=True)
    (Path(args.out) / "startup.json").write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out), flush=True)


def jobs(args):
    import runner

    corpus, _ = runner.load_corpus(Path(args.corpus))
    print(json.dumps(runner.enumerate_jobs(corpus)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    j = sub.add_parser("jobs")
    j.add_argument("--corpus", required=True)
    for name in ("fire", "startup"):
        p = sub.add_parser(name)
        p.add_argument("--engine", default="http://localhost:8002")
        p.add_argument("--engine-id", required=True, help="the engine container's full id")
        p.add_argument("--out", required=True)
        p.add_argument("--timeout", type=float, default=600 if name == "startup" else 120)
    f = sub.choices["fire"]
    f.add_argument("--jobs", required=True)
    f.add_argument("--parallel", type=int, default=1)
    f.add_argument("--pause", type=float, default=1.0)
    f.add_argument("--engine-note", default=None)
    args = ap.parse_args()
    return {"jobs": jobs, "fire": fire, "startup": startup}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
