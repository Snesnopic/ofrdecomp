# OFR container & bitstream format

As reverse-engineered from `libOptimFROG.0.dylib` v5.100. The official SDK ships its own
`SDK/Documentation/format.txt`; where the two disagree, what the binary does wins.

## Container layout

```
OFR  main_block   (fixed layout, see below)
HEAD head_block    (opaque passthrough, e.g. original WAV header bytes)
COMP* comp_block   (one or more compressed audio chunks)
TAIL tail_block    (opaque passthrough, e.g. trailing WAV bytes)
```

### Main block (`OFR `)

| Field | Size | Notes |
|---|---|---|
| magic | 4 | `"OFR "` (`0x2052464f` little-endian), `"OFRX"` for IEEE Float |
| size | 4 | main block size, 15 to 79 (`0x11` in current files, `0x0f` in older ones) |
| total_samples_low | 4 | low 32 bits of total sample count (values: frames × channels) |
| total_samples_high | 2 | high 16 bits |
| sample_type | 1 | `0/1`=8-bit, `2/3`=16-bit, `4/5`=24-bit, `6/7`=32-bit (even/odd = unsigned/signed), `8/9/10`=float32 (scaled to 1, 16 and 24 bits); anything else is rejected |
| channel_config | 1 | `0`=mono, `1`=stereo; anything else is rejected |
| samplerate | 4 | Hz |
| version+pad | 2 | `version = (val>>4) + 0x1194`; low 4 bits = pad |
| speedup+method | 1 | `speedup = val&7`, `method = val>>3` |
| min_decoder_version | 2 | only present if `size >= 0x11`, value + `0x1194`. Files that need more than 5100 are rejected |

The stream doesn't have to start at offset 0: the decoder scans the first megabyte for the magic
followed by a size below 256, which is how self-extracting files are opened. A size of exactly 12
marks a 4.5 alpha file, and `*RIF` at the start one even older. Neither is decodable; the official
library has no decoder for them either.

### HEAD / TAIL blocks

`"HEAD"` / `"TAIL"` magic + `uint32_t size` + `size` opaque bytes (typically the original
container's header/trailer bytes, e.g. a WAV RIFF header, passed through verbatim).

### COMP block (one per encoded chunk)

| Field | Size | Notes |
|---|---|---|
| magic | 4 | `"COMP"` (`0x504d4f43`) |
| compressed_size (`D`) | 4 | total bytes in this COMP block from the CRC field onward |
| crc32 | 4 | zlib CRC32 of the `D-4` bytes that follow this field |
| uncompressed_size | 4 | decoded **values** in this block (frames × channels) |
| sample_type_byte | 1 | mirrors the main block's sample_type for this chunk |
| channel_config_byte | 1 | mirrors channel_config |
| reserved | 2 | `pred_type = (reserved>>6)&0x1f`, `entropy_type = reserved>>11`, `post_type = reserved&0x3f` |
| encoder_id byte | 1 | skipped by the decoder; the range stream's dummy first byte doubles as a second ID byte |
| range-coded payload | `D-13` | the actual compressed audio, decoded via `OFR_RangeCoder` |

The next block starts at `block_start + 8 + D`, whatever the range coder consumed.

### Damage

The reference doesn't read the CRC while decoding. It notices damage some other way and carries on:

- the range decoder reads straight from the file, so a damaged block can read on past its payload
  into whatever follows it, and zeros past the end of the file;
- the next block is looked for at the current position; a bad magic there sets the
  recoverable-error flag and reading resumes at the stored position of the next block
  (`block_start + 8 + D`). If nothing is there either, the error is fatal. `TAIL` is found the
  same way;
- an invalid type, or an exception while decoding, turns the rest of the current slice and the
  rest of the block into silence and sets the flag. A slice is what one `OptimFROG_read` call
  asked for, clipped to the block, so the amount of silence depends on the caller's read size;
- a decoded sample outside the block's stored min/max counts as such an exception (predictor
  types 1 to 3 only), as does an `ent=2` escape wider than the bit depth.

A fatal error aborts the whole `OptimFROG_read` call with -1, even if part of it was decoded.

This decoder does the same, checked against the official library with
`tests/conformance/sdk_diff.sh` and a sweep of 560 single-bit damages over seven files covering
every predictor and entropy coder: 559 decode identically. The exception is a damaged final block
that reads past the end of the file: the official reader then serves stale bytes from its own buffer,
then whatever memory follows it, with no bounds check, while this decoder reads zeros. There is
also one deliberate difference: this decoder sets the flag when a block's CRC doesn't match,
without changing the output.

### Seeking

Blocks are independent, so seeking means walking block headers up to the one holding the target and
decoding forward from its start. The decoder remembers the blocks it has seen, so later seeks skip
the walk.

### Tags

Tags live after the OFR stream: an APEv2 tag (`APETAGEX`, version 2000) and an ID3v1 tag, in that
order. ID3v1 is read first, so a key also present in the APE tag takes the APE value and keeps the
ID3v1 spelling. Keys compare case-insensitively, at most 64 are kept, and binary APE items are
skipped.

### Dispatch (`reserved` word → algorithm selection)

- `pred_type`: `1`=LPC (`OFR_Predictor`/`OFR_PredictorStereo`), `2`=cascade NLMS alone, `3`=LPC and
  cascade NLMS alternating per segment (`OFR_PredictorCascadeMono`/`Stereo` for both),
  `4`=DualStream, see [dualstream_analysis.md](dualstream_analysis.md).
- `entropy_type`: `1`=fast (variance-exponent-indexed contexts), `2`=slow (master-tree + bit-width
  escape), `3`=ACM (integer context modeling).
- `post_type`: `1`=identity/scale (`mult`+`offset` per channel), `2`=value-remap (dense-index →
  sparse original value, for tonal/synthetic signals).

Any other value makes the block invalid. Types can change from one block to the next.

`data_bits` — the bit-width driving the predictor's shift-wrap trick and the entropy coder's
depth — is derived from the *post-processed* value range, not the container's raw
`sample_type` bit depth: `data_bits = ofr_bitlen(min, max)` over the (channel-combined, for
stereo) coded range. For `post=1` with a non-identity `mult`, this coded range is
`(min-offset)/mult .. (max-offset)/mult`, not the stored original min/max.

Whether `post=1` works in 32 or 64 bits depends on the min and max of both channels together: it
goes wide when `min < -0x800000` or `max >= 0x800000`, for reduction and reconstruction alike.

## Range coder

31-bit-window carryless-ish range coder (`OFR_RangeCoder`):

- Init: read a dummy byte (ignored), read a second byte into `cache`, `range=0x80`,
  `value=cache>>1`, then `normalize()`.
- `normalize()`: `while (range < 0x800001) { value = (cache&1)<<7 | value<<8; cache = next_byte();
  value |= cache>>1; range <<= 8; }`
- Uniform bits, tree-coded symbols (segment-tree frequency model with periodic halving), and a
  golomb-style helper are all built on top of this core loop — see
  `include/optimfrog_decoder.h`'s `OFR_RangeCoder` for the exact bit-for-bit transcription.

## Predictors, entropy coders, post-processors

See the dedicated deep-dive docs:

- [optimfrog_re_knowledge.md](optimfrog_re_knowledge.md) — algorithm overview across all
  components.
- [pred3_analysis.md](pred3_analysis.md) — cascade NLMS + LDLT final combiner (`pred=2` and `3`).
- [ent3_analysis.md](ent3_analysis.md) — ACM context-modeling entropy coder (`ent=3`).
- [post2_remap_analysis.md](post2_remap_analysis.md) — value-remap post-processor (`post=2`).
