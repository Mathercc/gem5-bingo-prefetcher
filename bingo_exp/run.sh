#!/bin/bash
# Usage: bingo_exp/run.sh <tag> <extra config args...>
# Runs every workload with the given config into m5out/bingo_exp/<tag>/<wl>.
# Same instruction window for every tag, so misses compare directly.
cd "$(dirname "$0")/.."
TAG=$1; shift
WARM=${WARM:-30000000}; MEAS=${MEAS:-30000000}
declare -A WL=(
  [stream]="--mode stream --bytes 16M --iters 60"
  [stride4]="--mode stride --bytes 16M --stride-lines 4 --iters 400"
  [chase]="--mode chase --bytes 16M --iters 60 --seed 1"
  [spatial]="--mode spatial --bytes 16M --iters 200 --seed 1"
  [pagepat]="--mode pagepat --bytes 1M --iters 4000 --seed 1"
)
# em3d as configured in the paper (400K nodes, degree 2, span 5, 15% remote).
# Building the graph takes ~317M instructions, so it is skipped on the atomic
# CPU; the detailed warmup then covers about two iterations (~10.8M each).
EM3D_ARGS="400000 2 5 15 100 1"
for w in ${WLS:-${!WL[@]} em3d}; do
  out=m5out/bingo_exp/$TAG/$w
  mkdir -p $out
  if [ $w = em3d ]; then
    run=(./em3d --fast-forward 330000000 --warmup-insts 22000000
         --argv="$EM3D_ARGS")
  else
    run=(./memwalk2 --warmup-insts $WARM --argv="${WL[$w]}")
  fi
  build/X86/gem5.opt -re -d $out configs/learning_gem5/part1/two_level_bingo.py \
    "${run[0]}" --maxinsts $MEAS "$@" "${run[@]:1}" > $out/log.txt 2>&1 &
done
wait
