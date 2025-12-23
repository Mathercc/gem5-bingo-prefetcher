# Bingo Spatial Prefetcher for gem5

Bingo (Bakhshalipour et al., HPCA 2019) as an L2 prefetcher for gem5.

Copy `src/mem/cache/prefetch/bingo.hh` and `bingo.cc` into the same directory of a gem5 tree, append the class in `patch/BingoPrefetcher.params.py` to `src/mem/cache/prefetch/Prefetcher.py`, and in that directory's `SConscript` add `'BingoPrefetcher'` to the SimObject list of `Prefetcher.py` and `Source('bingo.cc')`. Put `configs/learning_gem5/part1/two_level_bingo.py` (and the no-prefetch `two_level_nopf.py`) under the same path in gem5, and build `memwalk2.c` as a static binary in the gem5 root.
