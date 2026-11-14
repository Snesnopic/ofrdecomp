#!/usr/bin/env bash
# DualStream (.ofs) decoder conformance: the official ofs encodes at several qualities and modes,
# then ofs and this decoder decode, and the audio of both outputs must match byte for byte.
#   OFS: the official ofs tool; OFR_DECODER: this project's ofr
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OFS="${OFS:-$HOME/Downloads/OptimFROG_OSX_x64_5100/ofs}"
OFR_DECODER="${OFR_DECODER:-$ROOT/build/ofr}"
WORK="${WORK:-/tmp/conformance_dualstream}"
QUALITIES="${QUALITIES:-0 1 3 6 10}"
MODES="${MODES:-normal fast best}"

if [ ! -x "$OFS" ]; then
  echo "ofs not found at $OFS -- skipping DualStream conformance (set OFS=/path/to/ofs to enable)"
  exit 0
fi

rm -rf "$WORK"; mkdir -p "$WORK/tmp"
pass=0; fail=0; faillog="$WORK/failures.txt"; : > "$faillog"
note() { echo "FAIL: $*" >> "$faillog"; fail=$((fail+1)); }

# the official tools sometimes crash on exit under Rosetta: retry until the output exists
rtry_out() { local out="$1"; shift; local i; for i in 1 2 3; do rm -f "$out"; "$@" >/dev/null 2>&1; [ -s "$out" ] && return 0; done; return 1; }
data_chunk() {
  python3 -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
p = 12
while p + 8 <= len(d):
    tag, size = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
    if tag == b"data":
        sys.stdout.buffer.write(d[p + 8:p + 8 + size]); break
    p += 8 + size + (size & 1)
' "$1"
}

python3 "$ROOT/tests/conformance/gen_dualstream.py" "$WORK/wav"

for wav in "$WORK"/wav/*.wav; do
  base=$(basename "$wav" .wav)
  for mode in $MODES; do
    for q in $QUALITIES; do
      tag="$base m=$mode q=$q"
      ofs_file="$WORK/tmp/x.ofs" ref="$WORK/tmp/ref.wav" ours="$WORK/tmp/ours.wav"
      if ! rtry_out "$ofs_file" "$OFS" --encode --silent --overwrite --mode "$mode" --quality "$q" "$wav" --output "$ofs_file"; then
        note "$tag (no ofs output)"; continue
      fi
      rtry_out "$ref" "$OFS" --decode --silent --overwrite --writefreshheader "$ofs_file" --output "$ref" || { note "$tag (no ofs decode)"; continue; }
      rm -f "$ours"
      "$OFR_DECODER" --decode --silent --overwrite "$ofs_file" --output "$ours" >/dev/null 2>&1
      if [ ! -s "$ours" ]; then note "$tag (no output from our decoder)"; continue; fi
      if cmp -s <(data_chunk "$ref") <(data_chunk "$ours"); then pass=$((pass+1)); else note "$tag (audio differs)"; fi
    done
  done
done

echo "=== RESULTS ==="
echo "PASS=$pass FAIL=$fail"
if [ "$fail" -gt 0 ]; then echo "--- failures (first 30) ---"; head -30 "$faillog"; fi
[ "$fail" -eq 0 ]
