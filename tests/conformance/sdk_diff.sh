#!/usr/bin/env bash
# runs tests/conformance/sdk_probe.c against the official library and against this one, over a set
# of generated files, and diffs what the two print.
#   REFLIB: directory holding libOptimFROG.0.dylib (macOS x86_64) or libOptimFROG.so.0 (Linux)
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REFLIB="${REFLIB:-$ROOT}"
BUILD="${BUILD:-$ROOT/build}"
WORK="${WORK:-/tmp/sdk_diff}"
rm -rf "$WORK"; mkdir -p "$WORK/files"

if [ "$(uname)" = Darwin ]; then
  clang -arch x86_64 -I "$ROOT/include" "$ROOT/tests/conformance/sdk_probe.c" "$REFLIB/libOptimFROG.0.dylib" \
      -Wl,-rpath,"$REFLIB" -o "$WORK/probe_ref" || exit 1
else
  cc -I "$ROOT/include" "$ROOT/tests/conformance/sdk_probe.c" "$REFLIB/libOptimFROG.so.0" -Wl,-rpath,"$REFLIB" -o "$WORK/probe_ref" || exit 1
fi
cc -I "$ROOT/include" -c "$ROOT/tests/conformance/sdk_probe.c" -o "$WORK/probe.o" || exit 1
c++ "$WORK/probe.o" "$BUILD/libofrdecomp_lib.a" -lpthread -o "$WORK/probe_ours" || exit 1

python3 "$ROOT/tests/conformance/sdk_files.py" "$WORK/files" "$BUILD/ofr" "$BUILD/ofrenc" || exit 1
files=$(ls "$WORK"/files/*.ofr | sort)

# the official build sometimes crashes on exit under Rosetta: retry until the transcript is complete
for side in ref ours; do
  for i in 1 2 3; do
    "$WORK/probe_$side" "$WORK/out_$side.raw" $files > "$WORK/$side.txt" 2>/dev/null
    grep -q '^decodeFile unwritable=' "$WORK/$side.txt" && break
  done
done

if diff -u "$WORK/ref.txt" "$WORK/ours.txt" > "$WORK/diff.txt"; then
  echo "SDK transcripts identical ($(wc -l < "$WORK/ref.txt") lines)"
else
  echo "SDK transcripts differ:"
  head -80 "$WORK/diff.txt"
  exit 1
fi
