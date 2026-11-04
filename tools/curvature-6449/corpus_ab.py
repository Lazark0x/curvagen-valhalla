"""Derive corpus-v2-ab (Gate v2's blocks A + B only: 320 requests at K=12) from
tools/loopqual/corpus-v2.yaml, which stays unchanged. Needs pyyaml (the loopqual venv).
usage: python3 corpus_ab.py OUT.yaml"""
import sys
from pathlib import Path

import yaml

V1_ORIGINS = {"belgrade", "novisad", "zlatibor", "nis", "djerdap", "vlasina", "golija", "belacrkva"}
src = Path(__file__).resolve().parent.parent / "loopqual" / "corpus-v2.yaml"
c = yaml.safe_load(src.read_text())
c["id"] = "corpus-v2-ab"
c["cells"] = [x for x in c["cells"] if x["origins"] != "all" and not set(x["origins"]) & V1_ORIGINS]
yaml.safe_dump(c, open(sys.argv[1], "w"), sort_keys=False)
print(sum(len(x["origins"]) * len(x["distances_m"]) * len(x["seeds"]) for x in c["cells"]), "requests")
