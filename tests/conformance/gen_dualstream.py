#!/usr/bin/env python3
# WAV inputs for run_dualstream.sh: ordinary material, and full-scale material whose quantized
# reconstruction overshoots the sample range (the post-processor clamp case).
import math, os, random, struct, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
random.seed(99)

def wav(name, channels, bits, frames, gen):
    data = bytearray()
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    for i in range(frames):
        for c in range(channels):
            v = int(gen(i, c) * hi)
            data += max(lo, min(hi, v)).to_bytes(bits // 8, 'little', signed=True)
    align = channels * bits // 8
    fmt = b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, 44100, 44100 * align, align, bits)
    body = b'WAVE' + fmt + b'data' + struct.pack('<I', len(data)) + bytes(data)
    open(os.path.join(out, f'{name}__{channels}ch__{bits}bps.wav'), 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)

N = 60000
for ch in (1, 2):
    for bits in (16, 24):
        wav('multitone', ch, bits, N, lambda i, c: 0.3 * (math.sin(i * 0.031 + c) + math.sin(i * 0.0071) + 0.5 * math.sin(i * 0.37)))
        wav('noise', ch, bits, N, lambda i, c: random.uniform(-0.5, 0.5))
        wav('silence', ch, bits, 20000, lambda i, c: 0.0)
        wav('impulses', ch, bits, 20000, lambda i, c: 0.9 if i % 1000 == 0 else 0.0)
        wav('fullsine', ch, bits, N, lambda i, c: 1.05 * math.sin(i * 0.05 + c))
        wav('fullsquare', ch, bits, N, lambda i, c: 1.0 if (i // 40) % 2 else -1.0)
        wav('fullnoise', ch, bits, N, lambda i, c: random.choice((-1.0, 1.0)) * random.uniform(0.8, 1.0))
