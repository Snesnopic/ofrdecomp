#!/usr/bin/env python3
# long signals whose character changes along the way, so that an encoder splits them into many
# blocks and has reasons to pick different block types.
# output: <outdir>/<name>__<ch>ch__<bps>bps.raw (signed little-endian, interleaved)
import math, os, random, struct, sys

outdir = sys.argv[1]
os.makedirs(outdir, exist_ok=True)
random.seed(4321)
FRAMES = 1500000

def sample(i, c):
    section = (i // 200000) % 4
    t = float(i)
    if section == 0:
        return 0.5 * math.sin(t * 0.03 + c) + 0.2 * math.sin(t * 0.0071)
    if section == 1:
        return 0.3 * math.sin(t * 0.11) + random.uniform(-0.05, 0.05)
    if section == 2:
        return 0.0 if (i // 3000) % 2 else 0.7 * math.sin(t * 0.002 * (1 + c))
    return random.uniform(-0.6, 0.6) * (0.5 + 0.5 * math.sin(t * 0.0003))

for name, ch, bps in (("sections", 2, 16), ("sections", 1, 16), ("sections", 2, 24)):
    scale = float(1 << (bps - 1)) * 0.95
    width = bps // 8
    with open(os.path.join(outdir, f"{name}__{ch}ch__{bps}bps.raw"), 'wb') as f:
        buf = bytearray()
        for i in range(FRAMES):
            for c in range(ch):
                buf += struct.pack('<i', int(sample(i, c) * scale))[:width]
            if len(buf) > 1 << 20:
                f.write(buf)
                buf = bytearray()
        f.write(buf)
