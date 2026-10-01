#!/usr/bin/env python3
"""Summarise m5out/bingo_exp/<tag>/<wl>/stats.txt against the 'base' tag.

coverage (paper)   = misses removed / baseline misses
overprediction     = useless prefetches / baseline misses   (paper Fig. 7)
accuracy           = useful / issued
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(__file__), "..", "m5out", "bingo_exp")
WLS = ["stream", "stride4", "chase", "spatial", "pagepat"]


def stats(tag, wl):
    p = os.path.join(ROOT, tag, wl, "stats.txt")
    if not os.path.exists(p):
        return None
    d = {}
    for line in open(p):
        m = re.match(r"(\S+)\s+([-\d.e+naninf]+)", line)
        if m:
            d.setdefault(m.group(1), m.group(2))  # first dump only
    g = lambda k: float(d.get(k, "nan"))
    return dict(
        cpi=g("system.cpu.cpi"),
        miss=g("system.l2cache.demandMisses::total"),
        issued=g("system.l2cache.prefetcher.pfIssued"),
        useful=g("system.l2cache.prefetcher.pfUseful"),
        late=g("system.l2cache.prefetcher.pfLate"),
    )


tags = sys.argv[1:] or sorted(t for t in os.listdir(ROOT) if t != "base")
print(f"{'tag':14s}{'wl':9s}{'CPI':>8s}{'speedup':>9s}{'L2miss':>11s}"
      f"{'cover':>8s}{'acc':>7s}{'overpr':>8s}")
for wl in WLS:
    b = stats("base", wl)
    if not b:
        continue
    print(f"{'base':14s}{wl:9s}{b['cpi']:8.3f}{1:9.3f}{b['miss']:11.0f}")
    for t in tags:
        s = stats(t, wl)
        if not s:
            continue
        cov = (b["miss"] - s["miss"]) / b["miss"]
        acc = s["useful"] / s["issued"] if s["issued"] else float("nan")
        over = (s["issued"] - s["useful"]) / b["miss"]
        print(f"{t:14s}{wl:9s}{s['cpi']:8.3f}{b['cpi']/s['cpi']:9.3f}"
              f"{s['miss']:11.0f}{cov:8.1%}{acc:7.1%}{over:8.1%}")
