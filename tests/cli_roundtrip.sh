#!/usr/bin/env bash
# end-to-end check of the ofr tool: a WAV file has to come back byte for byte.
set -eu

OFR="$1"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK" <<'PY'
import math, struct, sys
work = sys.argv[1]
def wav(path, channels, bits, fmt=1, frames=30000, extra=b'LIST\x04\x00\x00\x00test'):
    data = bytearray()
    for i in range(frames):
        for c in range(channels):
            v = 0.5 * math.sin(i * 0.02 + c)
            if fmt == 3: data += struct.pack('<f', v)
            elif bits == 8: data.append(int(128 + v * 100))
            else: data += int(v * (1 << (bits - 2))).to_bytes(bits // 8, 'little', signed=True)
    align = channels * bits // 8
    head = b'fmt ' + struct.pack('<IHHIIHH', 16, fmt, channels, 44100, 44100 * align, align, bits)
    body = b'WAVE' + head + b'data' + struct.pack('<I', len(data)) + bytes(data) + extra
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
wav(work + '/s16.wav', 2, 16)
wav(work + '/m8.wav', 1, 8)
wav(work + '/s24.wav', 2, 24)
wav(work + '/f32.wav', 2, 32, fmt=3)
PY

for name in s16 m8 s24 f32; do
  for preset in 0 2 5; do
    "$OFR" --encode --silent --overwrite --md5 --preset "$preset" "$WORK/$name.wav" --output "$WORK/$name.ofr"
    "$OFR" --decode --silent --overwrite "$WORK/$name.ofr" --output "$WORK/$name.out.wav"
    cmp "$WORK/$name.wav" "$WORK/$name.out.wav"
    "$OFR" --verify "$WORK/$name.ofr"
    "$OFR" --check "$WORK/$name.ofr"
  done
done

# a raw stream has no stored container, so asking for WAV gets a fresh header
head -c 40000 /dev/urandom > "$WORK/noise.raw"
"$OFR" --encode --silent --raw --channelconfig STEREO_LR --sampletype SINT16 --rate 44100 "$WORK/noise.raw" --output "$WORK/noise.ofr"
"$OFR" --decode --silent --raw "$WORK/noise.ofr" --output "$WORK/noise.out.raw"
cmp "$WORK/noise.raw" "$WORK/noise.out.raw"
"$OFR" --decode --silent "$WORK/noise.ofr" --output "$WORK/noise.wav"
[ "$(wc -c < "$WORK/noise.wav")" -eq 40044 ]

# a damaged file has to fail loudly
python3 - "$WORK/s16.ofr" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[len(d) // 2] ^= 0x40
open(p, 'wb').write(d)
PY
if "$OFR" --verify "$WORK/s16.ofr" 2>/dev/null; then echo "--verify missed the damage"; exit 1; fi
if "$OFR" --decode --silent --overwrite "$WORK/s16.ofr" --output "$WORK/x.wav" 2>/dev/null; then echo "--decode missed the damage"; exit 1; fi
echo ok
