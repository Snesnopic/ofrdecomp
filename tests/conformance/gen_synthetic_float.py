#!/usr/bin/env python3
# Generate synthetic IEEE float32 raw PCM signals for the Float (.off) decoder conformance suite.
# Output: <outdir>/<name>__<ch>ch__float32.raw  (little-endian float32, interleaved)
import os, sys, struct, math, random

outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/conformance_float/synth"
os.makedirs(outdir, exist_ok=True)

def write(name, gen, ch, n):
    path = os.path.join(outdir, f"{name}__{ch}ch__float32.raw")
    with open(path, 'wb') as f:
        buf = bytearray()
        for i in range(n):
            for c in range(ch):
                buf += struct.pack('<f', gen(i, c))
        f.write(buf)

random.seed(5678)
N = 4000

for ch in (1, 2):
    write("silence",    lambda i,c: 0.0, ch, N)
    write("negzero",    lambda i,c: -0.0, ch, N)
    write("dc",         lambda i,c: 0.42, ch, N)
    write("dcneg",      lambda i,c: -0.33, ch, N)
    write("fullscale",  lambda i,c: 0.999999 if (i&1)==0 else -0.999999, ch, N)
    write("sine",       lambda i,c: 0.8*math.sin(i*0.05 + c), ch, N)
    write("sinesmall",  lambda i,c: 0.001*math.sin(i*0.05 + c), ch, N)
    write("sinetiny",   lambda i,c: 1e-20*math.sin(i*0.05 + c), ch, N)
    write("noise",      lambda i,c: random.uniform(-1.0, 1.0), ch, N)
    write("noisewide",  lambda i,c: random.uniform(-1e10, 1e10), ch, N)
    write("noisenear0", lambda i,c: random.uniform(-1e-30, 1e-30), ch, N)
    write("ramp",       lambda i,c: -1.0 + 2.0*i/N, ch, N)
    write("multitone",  lambda i,c: 0.3*(math.sin(i*0.05)+math.sin(i*0.131)+math.sin(i*0.011)), ch, N)
    write("chirp",      lambda i,c: 0.7*math.sin(i*i*1e-5 + c), ch, N)
    write("mixedmag",   lambda i,c: (10.0**random.randint(-30,10)) * random.choice([-1,1]) * random.uniform(0.1,1.0), ch, N)
    write("decay",      lambda i,c: 0.9*math.sin(i*0.05)*math.exp(-i/1000.0), ch, N)

print(f"{2 * 15} files written to {outdir}")
