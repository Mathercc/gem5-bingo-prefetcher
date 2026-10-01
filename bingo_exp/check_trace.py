#!/usr/bin/env python3
"""Replay a Bingo debug trace through an independent reference model.

Usage: gem5.opt --debug-flags=HWPrefetch --debug-file=trace.txt ... --pf bingo
       check_trace.py trace.txt [region_size history_entries history_assoc
                                 ft_entries at_entries vote_percent]

The model is written from the paper (Sec. IV): a filter table and an
accumulation table record each region's footprint from its trigger access
to the first eviction of any of its blocks; footprints are stored in one
history table indexed by PC+Offset and tagged with PC+Address; a trigger
uses a PC+Address hit, else blocks present in >= vote% of the PC+Offset
matches. It reads only the 'access' and 'evict' lines and checks that every
'trigger' line gem5 printed (the predicted footprint, minus the trigger
block) matches what the model predicts at that point.
"""
import re
import sys

args = [int(a) for a in sys.argv[2:]]
REGION, HIST, ASSOC, FT_N, AT_N, VOTE = (args + [2048, 16384, 16, 64, 128, 20][len(args):])
BLK = 64
NB = REGION // BLK
SETS = HIST // ASSOC
M64 = (1 << 64) - 1


def set_index(pc, off):
    x = ((pc << 6) ^ off) & M64
    x ^= x >> 33
    x = (x * 0xFF51AFD7ED558CCD) & M64
    x ^= x >> 33
    return x % SETS


class Table:
    """Fixed slots; victim = first empty slot, else least recently used."""

    def __init__(self, n):
        self.slots = [None] * n

    def victim(self):
        for i, e in enumerate(self.slots):
            if e is None:
                return i
        return min(range(len(self.slots)), key=lambda i: self.slots[i]["lru"])


tick = 0


def nt():
    global tick
    tick += 1
    return tick


ft, at = Table(FT_N), Table(AT_N)
hist = [Table(ASSOC) for _ in range(SETS)]


def find(t, region):
    for i, e in enumerate(t.slots):
        if e is not None and e["region"] == region:
            return i
    return None


def commit(a):
    s = hist[set_index(a["pc"], a["off"])]
    for i, e in enumerate(s.slots):
        if e and (e["pc"], e["region"], e["off"]) == (a["pc"], a["region"], a["off"]):
            break
    else:
        i = s.victim()
    s.slots[i] = dict(pc=a["pc"], region=a["region"], off=a["off"], fp=a["fp"], lru=nt())


def predict(pc, region, off):
    s = hist[set_index(pc, off)]
    for e in s.slots:
        if e and e["pc"] == pc and e["region"] == region and e["off"] == off:
            e["lru"] = nt()
            return e["fp"], "long"
    votes, m = [0] * NB, 0
    for e in s.slots:
        if e and e["pc"] == pc and e["off"] == off:
            e["lru"] = nt()
            m += 1
            for b in range(NB):
                votes[b] += (e["fp"] >> b) & 1
    if m == 0:
        return None, "miss"
    return sum(1 << b for b in range(NB) if votes[b] * 100 >= VOTE * m), "short"


def access(pc, addr):
    region, off = addr // REGION, (addr % REGION) // BLK
    i = find(at, region)
    if i is not None:
        at.slots[i]["fp"] |= 1 << off
        at.slots[i]["lru"] = nt()
        return None
    i = find(ft, region)
    if i is not None:
        f = ft.slots[i]
        if f["off"] != off:
            j = at.victim()
            if at.slots[j] is not None:
                commit(at.slots[j])
            at.slots[j] = dict(region=region, pc=f["pc"], off=f["off"],
                               fp=(1 << f["off"]) | (1 << off), lru=nt())
            ft.slots[i] = None
        return None
    j = ft.victim()
    ft.slots[j] = dict(region=region, pc=pc, off=off, lru=nt())
    fp, kind = predict(pc, region, off)
    if fp is None:
        return ("none", kind)
    return (fp & ~(1 << off), kind)


def evict(addr):
    region = addr // REGION
    i = find(at, region)
    if i is not None:
        commit(at.slots[i])
        at.slots[i] = None
    i = find(ft, region)
    if i is not None:
        ft.slots[i] = None


pat = re.compile(r"Bingo: (access pc=(0x[0-9a-f]+|0) addr=(0x[0-9a-f]+|0)"
                 r"|evict (0x[0-9a-f]+|0)|trigger (0x[0-9a-f]+|0) (none|pred=(0x[0-9a-f]+|0)))")
n = dict(access=0, evict=0, trigger=0, long=0, short=0, miss=0)
expect = None
bad = 0
for line in open(sys.argv[1]):
    m = pat.search(line)
    if not m:
        continue
    if m.group(2) is not None:
        if expect:
            bad += 1  # model saw a trigger that gem5 did not report
        n["access"] += 1
        expect = access(int(m.group(2), 16), int(m.group(3), 16))
        if expect:
            n[expect[1]] += 1
    elif m.group(4) is not None:
        if expect:
            bad += 1
            expect = None
        n["evict"] += 1
        evict(int(m.group(4), 16))
    else:
        n["trigger"] += 1
        got = "none" if m.group(6) == "none" else int(m.group(7), 16)
        want = expect[0] if expect else "<not a trigger>"
        if got != want:
            bad += 1
            if bad <= 5:
                print("MISMATCH", line.strip(), "model:", want if isinstance(want, str) else hex(want))
        expect = None
print(n, "mismatches:", bad)
sys.exit(1 if bad or n["trigger"] == 0 else 0)
