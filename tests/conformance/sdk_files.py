#!/usr/bin/env python3
# input files for sdk_diff.sh: WAV round trips (stored head and tail), several blocks, tags, damage.
import math, os, struct, subprocess, sys

out, ofr, ofrenc = sys.argv[1], sys.argv[2], sys.argv[3]

def wav(path, channels, bits, frames, extra=b''):
    data = bytearray()
    for i in range(frames):
        for c in range(channels):
            v = 0.5 * math.sin(i * 0.013 * (c + 1)) + 0.1 * math.sin(i * 0.37)
            data += int(v * (1 << (bits - 2))).to_bytes(bits // 8, 'little', signed=True)
    align = channels * bits // 8
    fmt = b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, 44100, 44100 * align, align, bits)
    body = b'WAVE' + fmt + b'data' + struct.pack('<I', len(data)) + bytes(data) + extra
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)

def encode_wav(name, channels, bits, frames, preset='2', extra=b''):
    w = os.path.join(out, name + '.wav')
    wav(w, channels, bits, frames, extra)
    o = os.path.join(out, name + '.ofr')
    subprocess.run([ofr, '--encode', '--silent', '--overwrite', '--preset', preset, w, '--output', o], check=True)
    os.remove(w)
    return o

def encode_raw(name, channels, bits, frames, env):
    r = os.path.join(out, name + '.raw')
    with open(r, 'wb') as f:
        for i in range(frames):
            for c in range(channels):
                v = 0.4 * math.sin(i * 0.021 + c) + 0.05 * math.sin(i * 0.9)
                f.write(int(v * (1 << (bits - 1))).to_bytes(bits // 8, 'little', signed=True))
    o = os.path.join(out, name + '.ofr')
    subprocess.run([ofrenc, r, o, '44100', str(channels), str(bits)], check=True, env=dict(os.environ, **env),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(r)
    return o

def ape(items):
    body = b''
    for key, value, flags in items:
        body += struct.pack('<II', len(value), flags) + key + b'\0' + value
    footer = b'APETAGEX' + struct.pack('<IIII', 2000, len(body) + 32, len(items), 0) + bytes(8)
    return body + footer

def id3(title, artist, year, genre, track=0):
    t = bytearray(128)
    t[0:3] = b'TAG'
    t[3:3 + len(title)] = title
    t[33:33 + len(artist)] = artist
    t[93:97] = year
    if track:
        t[126] = track
    t[127] = genre
    return bytes(t)

s16 = encode_wav('a_s16', 2, 16, 70000, extra=b'LIST\x06\x00\x00\x00abcdef')
encode_wav('b_m24', 1, 24, 30001, preset='5')
encode_wav('c_s32', 2, 32, 9000, preset='0')
encode_wav('d_m8', 1, 8, 5000)
encode_raw('e_blocks_p2', 2, 16, 40000, {'OFR_PRED': '2', 'OFR_ENT': '1', 'OFR_BLOCK': '7000'})
encode_raw('f_blocks_mix', 1, 16, 40000, {'OFR_BEST': '1', 'OFR_BLOCK': '9000'})
encode_raw('g_empty', 2, 16, 0, {})

data = open(s16, 'rb').read()
open(os.path.join(out, 'h_tagged.ofr'), 'wb').write(
    data + ape([(b'Title', b'  APE title ', 0), (b'ALBUM', b'An album', 0), (b'Cover', b'\x00\x01bin', 2), (b'Empty', b'', 0)])
    + id3(b'ID3 title', b'Some artist', b'2001', 17, 4))
open(os.path.join(out, 'i_id3only.ofr'), 'wb').write(data + id3(b'Only id3', b'X', b'1990', 200))

# damage: a flipped bit in the middle of a block, a broken block magic, a truncated tail
d = bytearray(data)
d[len(d) // 2] ^= 0x10
open(os.path.join(out, 'j_bitflip.ofr'), 'wb').write(d)
d = bytearray(data)
second = d.find(b'COMP', d.find(b'COMP') + 4)
if second > 0:
    d[second:second + 4] = b'XXXX'
open(os.path.join(out, 'k_badmagic.ofr'), 'wb').write(d)
open(os.path.join(out, 'l_truncated.ofr'), 'wb').write(data[:len(data) * 2 // 3])
open(os.path.join(out, 'm_junk.ofr'), 'wb').write(b'not an ofr file at all' * 10)
