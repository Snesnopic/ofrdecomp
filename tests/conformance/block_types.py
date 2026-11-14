#!/usr/bin/env python3
# prints the post/pred/ent types of every COMP block of an .ofr file, one line per block.
import struct, sys

d = open(sys.argv[1], 'rb').read()
pos = 8 + struct.unpack_from('<I', d, 4)[0]
pos += 8 + struct.unpack_from('<I', d, pos + 4)[0]
while d[pos:pos + 4] == b'COMP':
    size = struct.unpack_from('<I', d, pos + 4)[0]
    word = struct.unpack_from('<H', d, pos + 18)[0]
    print(f"post={word & 0x3f} pred={(word >> 6) & 0x1f} ent={word >> 11}")
    pos += 8 + size
