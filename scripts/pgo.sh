#!/usr/bin/env bash
# pgo.sh — build a profile-guided matcherbench (clang).
#   scripts/pgo.sh <bench-corpus-dir>      # e.g. ../matcher/bench
# Trains on the throughput mode of each workload present, merges the
# profile, and reconfigures build/ with -DMATCHER_PGO_PROFILE.
set -euo pipefail
cd "$(dirname "$0")/.."
CORPORA=${1:?usage: scripts/pgo.sh <bench-corpus-dir>}
GEN=build-pgo-gen
PROF=$(pwd)/$GEN/merged.profdata
cmake -S . -B "$GEN" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate" >/dev/null
cmake --build "$GEN" --target matcherbench -j >/dev/null
rm -f "$GEN"/*.profraw
for w in w1_1m w2 w3 w4 w5_1k w5_100k w5_1m; do
  [ -f "$CORPORA/$w.run.cmd.jsonl" ] || continue
  LLVM_PROFILE_FILE="$(pwd)/$GEN/$w.profraw" \
    "$GEN/matcherbench" "$CORPORA/$w" --mode throughput --reps 1 >/dev/null 2>&1
done
xcrun llvm-profdata merge -o "$PROF" "$GEN"/*.profraw 2>/dev/null \
  || llvm-profdata merge -o "$PROF" "$GEN"/*.profraw
cmake -S . -B build -DMATCHER_PGO_PROFILE="$PROF" >/dev/null
cmake --build build --target matcherbench -j >/dev/null
echo "pgo: build/matcherbench built with $PROF"
