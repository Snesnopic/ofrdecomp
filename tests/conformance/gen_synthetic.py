#!/usr/bin/env python3
# Generate a battery of pathological/edge synthetic raw PCM signals for conformance testing.
# Output: <outdir>/<name>__<ch>ch__<bps>bps.raw  (signed little-endian, interleaved)
import os, sys, struct, math, random

outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/conformance/synth"
os.makedirs(outdir, exist_ok=True)

def clampbits(v, bps):
    lo, hi = -(1 << (bps - 1)), (1 << (bps - 1)) - 1
    return max(lo, min(hi, v))

def pack(v, bps):
    b = struct.pack('<I', v & 0xffffffff)
    return b[:bps // 8]

def write(name, gen, ch, bps, n):
    path = os.path.join(outdir, f"{name}__{ch}ch__{bps}bps.raw")
    with open(path, 'wb') as f:
        for i in range(n):
            for c in range(ch):
                f.write(pack(clampbits(gen(i, c, bps), bps), bps))

random.seed(1234)
N = 8000
for ch in (1, 2):
    for bps in (8, 16, 24, 32):
        amp = (1 << (bps - 1)) - 1
        write("silence",   lambda i,c,b: 0, ch, bps, N)
        write("dc",        lambda i,c,b: amp//3, ch, bps, N)
        write("fullscale", lambda i,c,b: amp if (i&1)==0 else -amp-1, ch, bps, N)
        write("ramp",      lambda i,c,b,_a=amp: clampbits(-_a + (2*_a*i)//N, b), ch, bps, N)
        write("square",    lambda i,c,b,_a=amp: _a if (i//64)%2 else -_a, ch, bps, N)
        write("impulse",   lambda i,c,b,_a=amp: _a if (i%500)==0 else 0, ch, bps, N)
        write("noise",     lambda i,c,b,_a=amp: random.randint(-_a-1, _a), ch, bps, N)
        write("sine",      lambda i,c,b,_a=amp: int(_a*0.8*math.sin(i*0.05 + c)), ch, bps, N)
        write("nearzero",  lambda i,c,b: random.randint(-2, 2), ch, bps, N)
        write("multitone", lambda i,c,b,_a=amp: int(_a*0.3*(math.sin(i*0.05)+math.sin(i*0.131)+math.sin(i*0.011))), ch, bps, N)
# odd / tiny lengths (16-bit stereo)
for n in (1, 2, 3, 7, 1023, 1024, 1025):
    write(f"len{n}", lambda i,c,b: int(12000*math.sin(i*0.1+c)), 2, 16, n)
print("synthetic signals written to", outdir)
