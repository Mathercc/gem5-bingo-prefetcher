# Bingo Spatial Prefetcher for gem5

An implementation of Bingo (Bakhshalipour et al., *Bingo Spatial Data Prefetcher*, HPCA 2019) for gem5, attached to the L2 as a `Queued` prefetcher. The history starts from an initial implementation; the later commits rework it to follow the paper's design, fix how it is measured, check it against a reference model, and compare it with the paper's results.

## Installing and building

`./apply_to_gem5.sh ~/gem5` copies the files under `src/` and `configs/` into gem5, appends (or replaces) the parameter block from `patch/BingoPrefetcher.params.py` at the end of `src/mem/cache/prefetch/Prefetcher.py`, and registers the SimObject and `bingo.cc` in the `SConscript` next to it. Then build with `scons build/X86/gem5.opt -j$(nproc)`. Build the test programs as static binaries in the gem5 root with `gcc -O2 -std=c11 -static -o memwalk2 memwalk2.c` and `gcc -O2 -std=c11 -static -o em3d em3d.c`. Tested on gem5 v25.1.

## Design

A region (2KB by default) is recorded from its trigger access, the first access to it, until the first eviction or invalidation of any of its blocks, which is how the paper defines the end of residency. A region seen once waits in a 64-entry filter table; on a second, different block it moves to a 128-entry accumulation table that collects its footprint from demand accesses. The footprint is then stored in one 16K-entry, 16-way history table whose set is chosen by a hash of the trigger's PC+Offset and whose tag is the full PC+Address. On a trigger, a PC+Address match is used directly; otherwise every entry in that set with the same PC+Offset votes, and a block is predicted if at least 20% of them contain it. All predicted blocks are issued at once, nearest to the trigger first. The prefetcher trains on every L2 access, hits included, as the paper does for the LLC.

`use_long_event` and `use_short_event` exist only for the ablation below; `max_prefetches` caps the blocks issued per trigger (0, the default, issues the whole footprint).

## Experiments

`bingo_exp/run.sh <tag> [config options]` runs the five access patterns and em3d in parallel, each on a fixed instruction window, and writes to `m5out/bingo_exp/<tag>/`. Run without options it is the no-prefetcher baseline (use the tag `base`, or `base_o3` with `--cpu o3`). `--pf bingo|sms|ampm|bop|spp` selects the L2 prefetcher; the others are gem5's built-in versions of the paper's competitors, and all of them see the same accesses. `bingo_exp/summarize.py` reports, against the matching baseline, the coverage (fraction of baseline misses removed), accuracy, overprediction (useless prefetches divided by baseline misses, as in Fig. 7 of the paper), and the share of baseline misses that caught a prefetch still in flight. The system is an X86 TimingSimpleCPU (or O3CPU) with a 64KB L1D, a 256KB L2 and DDR3-1600.

Of the five patterns, stream reads every line, stride4 reads one line in four, and chase is a random pointer chase. spatial uses the same irregular set of lines in every page, which PC+Offset can learn, while pagepat gives each page its own fixed set and a random starting line, which only PC+Address can learn. These five run 30M instructions of warmup and 30M measured.

em3d is the one workload the paper itself uses that can run here. It is an electromagnetic wave propagation kernel over a bipartite graph: two sets of individually allocated nodes, each reading the values of its neighbours in the other set through pointers. `em3d.c` is written from the description of Olden em3d and uses the paper's configuration from Table II: 400K nodes, degree 2, span 5 (85% of neighbours lie within five nodes of a node's own index) and 15% remote (the rest are anywhere in the graph). Building the graph takes about 317M instructions, so the run skips it on gem5's atomic CPU (`--fast-forward 330000000`), then switches to the detailed CPU for 22M instructions of warmup, about two iterations, and 30M measured.

## Verification

The prefetcher prints a trace under `--debug-flags=HWPrefetch`: every access it observes, every eviction, and the footprint it predicts at each trigger. `bingo_exp/check_trace.py` replays the accesses and evictions through a separate Python model written from Section IV of the paper and compares every prediction. Over 12M instructions of each of the five patterns (about 1.08M triggers, 63K of them PC+Address hits and 987K PC+Offset votes) there were no mismatches. As a negative control, the same pagepat trace replayed with one parameter wrong in the model (vote threshold 50%, 8 ways, a 32-entry filter table, a 64-entry accumulation table, or 4KB regions) gave between 7,647 and 110,176 mismatches each, so the check does detect a wrong table size or policy.

The aggregate counters agree as well: on stride4 each 2KB region has 8 accessed blocks, and the run shows 468,745 triggers and exactly 7 × 468,745 predicted blocks.

## Results

The full numbers are in `bingo_exp/results.txt`.

| workload | coverage / accuracy | speedup (TimingSimple) | speedup (O3) |
|---|---|---|---|
| stream | 96.9% / 100% | 2.17× | 2.27× |
| stride4 | 87.5% / 100% | 1.99× | 2.09× |
| spatial | 90.3% / 99.2% | 1.93× | 2.25× |
| pagepat | 91.0% / 55.8% | 1.89× | 2.19× |
| chase | 0.3% / 4.1% (overprediction 6.8%) | 1.01× | 1.01× |
| em3d | 83.1% / 64.5% | 1.71× | 1.64× |

The misses Bingo leaves on the regular patterns are exactly the first access to each region, which no spatial prefetcher can avoid. On the O3 core some prefetches arrive late: on stride4, 28.1% of baseline misses catch a prefetch still in flight, which gem5 counts as a miss and the prefetch as unused, so the table shows 59.4% coverage and 28.1% overprediction; adding the late ones back gives the same 87.5%, with no real overprediction.

## Comparison with the paper

The paper evaluates four out-of-order cores with an 8MB LLC on server, scientific and SPEC workloads in ChampSim; this is one core with a 256KB L2 in gem5. Of its workloads only em3d can be run here, so it is the direct comparison. The table puts our em3d numbers next to the paper's. The paper prints its em3d speedups on Fig. 8; its coverages are only drawn on Fig. 7, so they are read off the bars and are approximate. VLDP has no gem5 version.

| prefetcher | paper coverage (Fig. 7) | paper speedup (Fig. 8) | our coverage | our overprediction | our speedup |
|---|---|---|---|---|---|
| BOP | ~82% | +179% | 83.8% | 13.2% | +82% |
| SPP | ~80% | +165% | 84.0% | 404% | +80% |
| AMPM | ~87% | +221% | 84.9% | 993% | +79% |
| SMS | ~87% | +218% | 82.6% | 24.4% | +73% |
| Bingo | ~94% | +285% | 83.1% | 45.8% | +71% |

Our em3d reproduces the paper's picture of the workload but not Bingo's lead on it. As in the paper, em3d is the most memory-bound workload (baseline 85 L2 misses per thousand instructions here, 32.4 LLC MPKI in the paper) and every spatial prefetcher removes most of its misses, all of them inside a narrow band (80% to 85% here, about 80% to 94% in the paper). Bingo's coverage of 83.1% is in that band, but it is not the highest, its overprediction is higher than BOP's and SMS's, and its speedup is the lowest of the five, where the paper has it first by a wide margin. Bingo's speedup is a quarter of the paper's and the others' are a third to a half of theirs, which one core with a simple timing model, a small L2 and no bandwidth contention cannot be expected to match; on the O3 core Bingo gives +64%, behind SPP's +88%.

The reason Bingo loses its lead is visible in the counters. Only 1.1% of its em3d triggers (4.4K of 401K) find a PC+Address match; with `--bingo-events short` the coverage, 83.1%, is unchanged, and with `--bingo-events long` it drops to zero, so here Bingo is effectively SMS with 20% voting. em3d's nodes and edge arrays span about 48MB, roughly 23K 2KB regions, revisited once per iteration, while the history table is indexed by PC+Offset and the kernel has only a handful of load PCs, so most of the 16K entries are never reachable and a region's footprint is overwritten long before it is visited again. The voting then merges the footprints of different regions, which is where the 45.8% overprediction comes from. Whether the paper's system avoided this through its larger LLC, its own em3d build or something else cannot be told from the paper.

The microbenchmarks test three of the paper's other claims more cleanly; two hold and one holds in part.

The first is the claim behind Fig. 2 and Fig. 3: PC+Address alone is accurate but rarely matches, PC+Offset matches often but is less accurate, and using both keeps the coverage of the short event with better accuracy. On pagepat, PC+Address alone gives 39.9% coverage at 99.9% accuracy, PC+Offset alone 90.2% at 42.1%, and Bingo 91.0% at 55.8%, with overprediction falling from 124% to 72%. On the patterns where PC+Offset is already exact (stream, stride4, spatial) the two events give the same prediction, which is the redundancy the paper measures in Fig. 4 and the reason it stores each footprint only once.

The second is Fig. 6: coverage grows with history size and plateaus by 16K entries. Here it rises from 90.1% at 1K entries to 91.0% at 16K on pagepat and stays there at 32K and 64K. The curve is flat because these tests have a single load PC, so this is weak support.

The third is Fig. 7, where Bingo has the highest coverage on every workload while its overprediction is no worse than the others'. On the microbenchmarks this holds only in part, and on em3d, as above, it does not. Against gem5's SMS, AMPM, BOP and SPP, Bingo has the highest coverage on pagepat and ties SPP on spatial, but AMPM covers more of stream (98.4% against 96.9%) and stride4 (93.3% against 87.5%), at 7.5% and 35.5% accuracy, and SPP covers 1.0% of chase against Bingo's 0.3%. The overprediction side holds clearly: Bingo stays at or below 0.7% on stream, stride4 and spatial, against 169% to 1214% for AMPM and 344% to 856% for SPP. The paper's prefetcher closest to Bingo is SMS. gem5's SMS keeps only 64 active generations, fewer than the 128 regions the L2 holds, so in its default setting it drops every stream and stride4 generation before it ends and learns nothing; with 256 entries it matches Bingo exactly on those two, as it should, and Bingo's lead appears where the paper puts it, on pagepat (91.0% against 82.7% coverage, 72% against 116% overprediction) and on spatial accuracy (99.2% against 76.6%).

Two results run against the paper's picture and are left as they are. The pagepat accuracy is limited by history capacity under a single PC: the table is indexed by PC+Offset, so only a few sets are usable, and raising the associativity to 64 lifts the PC+Address hit rate from 49% to 74% and accuracy to 70%. On these page-wide tests 4KB regions also beat 2KB (stride4 93.8%), but the default stays at the paper's 2KB because that is what matches its 119KB storage figure for 16K entries. Separately, SMS speeds chase up by about 10% with under 1% coverage and over 100% overprediction; that effect was not investigated.
