#!/usr/bin/env bash
# IEEE Float (.off) decoder conformance suite -- direction A only (decode-side): the official
# `off` encoder produces the .ofr file, our decoder decodes it, and the result must equal the
# original raw float32 PCM bit-for-bit. No reference *decoder* is needed here (unlike the
# lossless suite) since libOptimFROG's shared library cannot open .off files at all -- comparing
# against the original raw samples directly is both necessary and sufficient. We don't have a
# Float encoder yet, so there is no direction B for this format.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OFF="${OFF:-$HOME/Downloads/OptimFROG_OSX_x64_5100/off}"
OFR="${OFR_DECODER:-$ROOT/build/ofr}"
WORK="${WORK:-/tmp/conformance_float}"

mkdir -p "$WORK/synth" "$WORK/tmp"
pass=0; fail=0; faillog="$WORK/failures.txt"; : > "$faillog"
note() { echo "FAIL: $*" >> "$faillog"; fail=$((fail+1)); }
ok()   { pass=$((pass+1)); }

# retry a command up to 3x, succeeding only when the sentinel output file is non-empty
# (Rosetta/emulation flakiness on the official binary; a real mismatch is never hidden).
rtry_out() { local out="$1"; shift; local i; for i in 1 2 3; do rm -f "$out"; "$@" >/dev/null 2>&1; [ -s "$out" ] && return 0; done; return 1; }

cc_of() { [ "$1" = 2 ] && echo STEREO_LR || echo MONO; }

if [ ! -x "$OFF" ]; then
  echo "off binary not found at $OFF -- skipping Float conformance (set OFF=/path/to/off to enable)"
  exit 0
fi

echo "=== preparing float synthetic corpus ==="
python3 "$ROOT/tests/conformance/gen_synthetic_float.py" "$WORK/synth" >/dev/null

echo "=== running (dir A only: off encode -> our decode -> must equal original) ==="
for raw in "$WORK"/synth/*__*ch__float32.raw; do
  [ -f "$raw" ] || continue
  base=$(basename "$raw")
  ch=$(echo "$base" | sed -E 's/.*__([0-9]+)ch__.*/\1/')
  cc=$(cc_of "$ch")

  of="$WORK/tmp/a.off.ofr" ours="$WORK/tmp/a_our.raw"
  rtry_out "$of" "$OFF" --encode --raw --channelconfig "$cc" --sampletype FLOAT32_1 --rate 44100 "$raw" --output "$of" --overwrite
  if [ ! -s "$of" ]; then note "enc $base (no off output)"; continue; fi

  rm -f "$ours"
  rtry_out "$ours" "$OFR" --decode --raw "$of" --output "$ours" --overwrite
  if [ ! -s "$ours" ]; then note "dec $base (no our-decoder output)"; continue; fi

  if ! cmp -s "$raw" "$ours"; then note "$base (ours != original)"; else ok; fi
done

echo "=== RESULTS ==="
echo "PASS=$pass FAIL=$fail"
if [ "$fail" -gt 0 ]; then echo "--- failures ---"; cat "$faillog"; fi
[ "$fail" -eq 0 ]
