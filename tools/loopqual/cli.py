#!/usr/bin/env python3
"""loopqual — loop-quality eval harness for the curvagen round-trip action
(wayfinder #48). See README.md for the metric spec (v1) and usage."""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare as compare_mod
import runner


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="loopqual",
        description="Loop-quality eval harness: run a corpus against a routing "
                    "endpoint and report versioned defect metrics; compare runs.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="fire a corpus, compute metrics, write reports")
    r.add_argument("--engine", required=True,
                   help="base URL (engine mode: fork /route; --serving: orchestrator /round-trip)")
    r.add_argument("--corpus", required=True, help="corpus YAML/JSON file")
    r.add_argument("--out", required=True, help="output dir (responses/, loops.jsonl, report.*)")
    r.add_argument("--serving", action="store_true",
                   help="hit the orchestrator app DTO (POST /round-trip) instead of the fork /route")
    r.add_argument("--trace-engine", default=None,
                   help="base URL for the /trace_attributes way-reuse pass "
                        "(default: --engine in engine mode; disabled in serving mode)")
    r.add_argument("--workers", type=int, default=None,
                   help="parallel requests (default: 3 engine mode, 1 serving mode)")
    r.add_argument("--no-way", action="store_true", help="skip the edge_reuse_way pass")
    r.add_argument("--engine-note", default="",
                   help="free-text engine provenance stamped into responses and reports")
    r.set_defaults(func=runner.run)

    c = sub.add_parser("compare", help="per-metric delta table between two report.json files")
    c.add_argument("baseline")
    c.add_argument("candidate")
    c.set_defaults(func=lambda a: compare_mod.compare(a.baseline, a.candidate))

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
