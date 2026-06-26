#!/usr/bin/env python3
# Generate a large battery of pathological/edge synthetic raw PCM signals for conformance testing.
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
        buf = bytearray()
        for i in range(n):
            for c in range(ch):
                buf += pack(clampbits(gen(i, c, bps), bps), bps)
        f.write(buf)

random.seed(1234)
N = 8000

# ---- core battery: all signal types x all bps x mono/stereo ----
for ch in (1, 2):
    for bps in (8, 16, 24, 32):
        amp = (1 << (bps - 1)) - 1
        write("silence",   lambda i,c,b: 0, ch, bps, N)
        write("dc",        lambda i,c,b,_a=amp: _a//3, ch, bps, N)
        write("dcneg",     lambda i,c,b,_a=amp: -_a//3, ch, bps, N)
        write("dcoff",     lambda i,c,b,_a=amp: _a//2 + 7, ch, bps, N)          # asymmetric min/max
        write("fullscale", lambda i,c,b,_a=amp: _a if (i&1)==0 else -_a-1, ch, bps, N)
        write("althalf",   lambda i,c,b,_a=amp: _a//2 if (i&1)==0 else -_a//2, ch, bps, N)
        write("ramp",      lambda i,c,b,_a=amp: clampbits(-_a + (2*_a*i)//N, b), ch, bps, N)
        write("rampslow",  lambda i,c,b,_a=amp: clampbits(-_a + (2*_a*(i%1000))//1000, b), ch, bps, N)
        write("square",    lambda i,c,b,_a=amp: _a if (i//64)%2 else -_a, ch, bps, N)
        write("square7",   lambda i,c,b,_a=amp: _a if (i//7)%2 else -_a, ch, bps, N)
        write("impulse",   lambda i,c,b,_a=amp: _a if (i%500)==0 else 0, ch, bps, N)
        write("impulseneg",lambda i,c,b,_a=amp: (-_a-1) if (i%333)==0 else 0, ch, bps, N)
        write("step",      lambda i,c,b,_a=amp: -_a if i < N//2 else _a, ch, bps, N)
        write("noise",     lambda i,c,b,_a=amp: random.randint(-_a-1, _a), ch, bps, N)
        write("noisegauss",lambda i,c,b,_a=amp: clampbits(int(sum(random.uniform(-1,1) for _ in range(6))/6*_a), b), ch, bps, N)
        write("sine",      lambda i,c,b,_a=amp: int(_a*0.8*math.sin(i*0.05 + c)), ch, bps, N)
        write("sineful",   lambda i,c,b,_a=amp: int(_a*0.999*math.sin(i*0.05 + c)), ch, bps, N)
        write("nearzero",  lambda i,c,b: random.randint(-2, 2), ch, bps, N)
        write("tinyamp",   lambda i,c,b: random.randint(-1, 1), ch, bps, N)
        write("lowtone",   lambda i,c,b,_a=amp: int(max(4,_a*0.0008)*math.sin(i*0.05)), ch, bps, N)   # triggers post=2 active remap
        write("multitone", lambda i,c,b,_a=amp: int(_a*0.3*(math.sin(i*0.05)+math.sin(i*0.131)+math.sin(i*0.011))), ch, bps, N)
        write("chirp",     lambda i,c,b,_a=amp: int(_a*0.7*math.sin(i*i*1e-5 + c)), ch, bps, N)
        write("dither",    lambda i,c,b,_a=amp: clampbits(int(_a*0.5*math.sin(i*0.07))+random.randint(-3,3), b), ch, bps, N)

# ---- stereo cross-channel pathologies (16 + 24 bit) ----
for bps in (16, 24):
    amp = (1 << (bps - 1)) - 1
    write("st_mono",     lambda i,c,b,_a=amp: int(_a*0.7*math.sin(i*0.05)), 2, bps, N)                # L==R
    write("st_antiphase",lambda i,c,b,_a=amp: int(_a*0.7*math.sin(i*0.05))*(1 if c==0 else -1), 2, bps, N)  # L=-R
    write("st_indep",    lambda i,c,b,_a=amp: random.randint(-_a-1,_a), 2, bps, N)                    # decorrelated
    write("st_rampdiff", lambda i,c,b,_a=amp: clampbits(-_a + (2*_a*i)//N if c==0 else _a-(2*_a*i)//N, b), 2, bps, N)
    write("st_onesilent",lambda i,c,b,_a=amp: (int(_a*0.7*math.sin(i*0.05)) if c==0 else 0), 2, bps, N)
    write("st_quietloud",lambda i,c,b,_a=amp: (int(_a*0.9*math.sin(i*0.05)) if c==0 else int(8*math.sin(i*0.05))), 2, bps, N)

# ---- exact-period tonal (stresses post=2 remap + stereo LDLT re-solve) ----
for per in (16, 32, 64, 100, 128, 256, 441):
    amp = 32767
    write(f"tone_p{per}", (lambda P: lambda i,c,b: int(20000*math.sin(2*math.pi*i/P + c)))(per), 2, 16, N)

# ---- many boundary lengths (16-bit stereo tonal) ----
lengths = [1,2,3,4,5,6,7,8,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257,
           511,512,513,1023,1024,1025,2047,2048,2049,4095,4096,4097,
           8191,8192,8193,65535,65536,65537]
for n in lengths:
    write(f"len{n}", lambda i,c,b: int(12000*math.sin(i*0.1+c)), 2, 16, n)

# ---- a couple of large multi-feature signals (longer, to exercise weight updates) ----
for n in (50000,):
    amp = 32767
    write(f"big_music{n}", lambda i,c,b,_a=amp: int(_a*0.6*(math.sin(i*0.05+c)+0.3*math.sin(i*0.211))+random.randint(-20,20)), 2, 16, n)
    write(f"big_noise{n}", lambda i,c,b,_a=amp: random.randint(-_a-1,_a), 2, 16, n)

print("synthetic signals written to", outdir)
