#!/usr/bin/env python3
"""Summarise m5out/bingo_exp/<tag>/<wl>/stats.txt against the 'base' tag.

coverage (paper)   = misses removed / baseline misses
overprediction     = useless prefetches / baseline misses   (paper Fig. 7)
accuracy           = useful / issued
late               = extra demand MSHR hits over the baseline / baseline
                     misses: demands that caught a prefetch still in flight.
                     gem5 counts them as misses and the prefetch as unused
                     (pfLate stays 0 on this path), so they matter with O3.
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
        # gem5 omits counters that stayed at zero
        mshr_hits=float(d.get("system.l2cache.demandMshrHits::total", 0)),
    )


tags = sys.argv[1:] or sorted(t for t in os.listdir(ROOT)
                               if not t.startswith("base"))
print(f"{'tag':14s}{'wl':9s}{'CPI':>8s}{'speedup':>9s}{'L2miss':>11s}"
      f"{'cover':>8s}{'acc':>7s}{'overpr':>8s}{'late':>7s}")
for wl in WLS:
    shown = set()
    for t in tags:
        # tags ending in _o3 are compared with the out-of-order baseline
        bt = "base_o3" if t.endswith("_o3") else "base"
        b, s = stats(bt, wl), stats(t, wl)
        if not b or not s:
            continue
        if bt not in shown:
            shown.add(bt)
            print(f"{bt:14s}{wl:9s}{b['cpi']:8.3f}{1:9.3f}{b['miss']:11.0f}")
        cov = (b["miss"] - s["miss"]) / b["miss"]
        acc = s["useful"] / s["issued"] if s["issued"] else float("nan")
        over = (s["issued"] - s["useful"]) / b["miss"]
        late = (s["mshr_hits"] - b["mshr_hits"]) / b["miss"]
        print(f"{t:14s}{wl:9s}{s['cpi']:8.3f}{b['cpi']/s['cpi']:9.3f}"
              f"{s['miss']:11.0f}{cov:8.1%}{acc:7.1%}{over:8.1%}{late:7.1%}")
