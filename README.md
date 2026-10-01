# Bingo Spatial Prefetcher for gem5

Bingo (Bakhshalipour et al., HPCA 2019) as an L2 prefetcher for gem5.

`./apply_to_gem5.sh ~/gem5` copies the files under `src/` and `configs/` into gem5, appends (or replaces) the parameter block from `patch/BingoPrefetcher.params.py` at the end of `src/mem/cache/prefetch/Prefetcher.py`, and registers the SimObject and `bingo.cc` in the `SConscript` next to it. Then build with `scons build/X86/gem5.opt -j$(nproc)`, and build `memwalk2.c` as a static binary in the gem5 root. `bingo_exp/run.sh` runs the experiments and `bingo_exp/summarize.py` tabulates them.
