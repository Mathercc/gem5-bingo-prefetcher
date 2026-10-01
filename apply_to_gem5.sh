#!/bin/bash
# Re-apply the restored Bingo prefetcher onto a gem5 checkout.
# Idempotent: safe to run twice.   Usage: ./apply_to_gem5.sh [/path/to/gem5]
set -e
GEM5=${1:-/home/a/gem5}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -f "$GEM5/SConstruct" ] || { echo "not a gem5 tree: $GEM5"; exit 1; }

cp -v "$HERE/src/mem/cache/prefetch/bingo.cc" "$HERE/src/mem/cache/prefetch/bingo.hh" \
      "$GEM5/src/mem/cache/prefetch/"
cp -v "$HERE/configs/learning_gem5/part1/two_level_bingo.py" \
      "$HERE/configs/learning_gem5/part1/two_level_nopf.py" \
      "$GEM5/configs/learning_gem5/part1/"
cp -v "$HERE/memwalk2.c" "$GEM5/"
mkdir -p "$GEM5/bingo_exp" && cp -v "$HERE"/bingo_exp/* "$GEM5/bingo_exp/" 2>/dev/null || true

python3 - "$GEM5" "$HERE" <<'PY'
import sys, io, re
gem5, here = sys.argv[1], sys.argv[2]

# 1) Prefetcher.py: append (or replace) the SimObject param block
p = gem5 + '/src/mem/cache/prefetch/Prefetcher.py'
s = open(p).read()
block = open(here + '/patch/BingoPrefetcher.params.py').read().rstrip() + '\n'
if 'class BingoPrefetcher' in s:
    # the block is always appended last, so replace it to pick up new params
    s = s[:s.index('class BingoPrefetcher')].rstrip('\n')
    print('Prefetcher.py: replaced BingoPrefetcher')
else:
    print('Prefetcher.py: added BingoPrefetcher')
open(p, 'w').write(s.rstrip('\n') + '\n\n\n' + block)

# 2) SConscript: register the SimObject and the source file
p = gem5 + '/src/mem/cache/prefetch/SConscript'
s = open(p).read()
if 'BingoPrefetcher' not in s:
    s = s.replace("'FetchDirectedPrefetcher'\n", "'FetchDirectedPrefetcher', 'BingoPrefetcher'\n", 1)
    print('SConscript: registered SimObject')
if "Source('bingo.cc')" not in s:
    s = s.rstrip('\n') + "\nSource('bingo.cc')\n"
    print("SConscript: added Source('bingo.cc')")
open(p, 'w').write(s)
PY
echo "done.  build:  cd $GEM5 && scons build/X86/gem5.opt -j\$(nproc)"
