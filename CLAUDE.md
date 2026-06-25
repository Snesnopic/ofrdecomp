# optimfrogcpp

C++ decompilation and rewrite of the OptimFROG decoder (closed-source).
Reference binary: `libOptimFROG.dylib` (v5.100, macOS x64) in the project root.
Part of the chisel ecosystem.

## Structure

```
src/
  main.cpp
  optimfrog.cpp                        — top-level decode pipeline
  optimfrog_decoder.cpp                — block decode orchestration
  optimfrog_decoder_entropy.cpp        — entropy decoding (RangeCoder, Huffman)
  optimfrog_decoder_predictor.cpp      — predictor (mono complete; stereo in progress)
include/
  OptimFROG.h                          — public API (from original header)
  OptimFROGDecoder.h                   — decoder interface
  optimfrog_decoder.h                  — internal decoder structs
  optimfrog_tables.h
  PortableTypes.h, SystemID.h
```

No CMakeLists.txt at time of move — build system uses the root Makefile.

## RE methodology

Analysis of `libOptimFROG.dylib` is done dynamically via LLDB Python scripts
(`test_lldb_*.py` in project root). Scripts extract internal variable state and
intermediate vectors during real binary execution, then compare against C++ output
sample-by-sample. Ghidra is also available for static RE (`libofr.asm`, `dump.asm`).

Reference files:
- `sine_mono.ofr` — mono test file (validated)
- `test_stereo.ofr` — stereo test file (176 KB, in progress)

## Current state (updated 2026-06-24 — stereo bit-exact)

### Working — verified bit-exact

- **Full mono pipeline**: Entropy + Predictor for mono files produces MD5-identical
  output to the reference binary on `sine_mono.ofr` (MD5: `4c820520d7faa9b2e11c76e9c4d2d7c7`).
- **LDLT solver (Cholesky)**: `solveCholeskyLeft` and `solveCholeskyRight` write the
  solution vector in-place onto the input array (`m_left_coefs`/`m_right_coefs`),
  using `m_temp_vector` only as scratchpad.
- **Predictor damping formula**: damping = `(W-1)/W` not `W`. Binary `FUN_00014dc0`
  receives `(-1.0 + W)/W`. Passing `W` caused exponential divergence at weight-update.
- **inst_matrix never updated**: `FUN_00014e30` does not write to inst_matrix after the
  initial setup at `sample_count == order+1`. Removed stale update in `update_weights()`.

- **Full stereo pipeline** (`test_stereo.ofr`, pred_type=1 / entropy_type=1 / post_type=2):
  bit-exact, MD5 `63c303b47bda85ba2a0029f71cfba1d5`. Uses `OFR_PredictorStereo` (integrated
  cross-channel LDLT, NOT a separate cc predictor) + the **fast stereo entropy** decoder.

### Stereo decode details (all verified against the binary)

- **Dispatch** (`FUN_00010870`): for our file the RC read order is PostProcessor(post_type=2,
  `FUN_00017c40`) → predictor `init`(`FUN_00007130`, reads weight/interval/order) → entropy
  `init`(`FUN_00004510`, reads one weight). `init_from_bitstream` on the stereo predictor is a
  genuine no-op — all RC reads happen in `init`.
- **Predictor min/max/shift** come from the post-processor via `FUN_00007400`; entropy/predictor
  `bit_depth` = `local_1c` = `FUN_00018d80(min,max)` (data bit-length), not `bitspersample`.
- **Fast stereo entropy** (`FUN_00004710`/`FUN_00005ef0`): per-channel variance, context array
  indexed by the **variance exponent** (`bits(var)>>52 - 1023`); `num_contexts = bit_depth*2`,
  per-context `num_symbols = bit_depth<4 ? 1<<bd : bd*8-0x10`; var update `var = sym²·w2 + w2 + var·w`.
  Initial variance = 1.0. This is a different decoder from the mono slow path (`FUN_00109ab0`).
- **Stereo decode loop** (`FUN_00007440`): clamp prediction to [min,max], `((clamped+res)<<shift)>>shift`,
  update with the final value. `lrint` rounding (cvtsd2si).

### Encoder

- **Not started.** Decoder is the only thing implemented.

## Test infrastructure (reference encoder available!)

`~/Downloads/OptimFROG_OSX_x64_5100/` has the official `ofr` CLI (x86_64, runs under Rosetta)
+ `SDK/Documentation/format.txt` (full format spec). Generate test files with any preset:
`ofr --encode --raw --channelconfig {MONO|STEREO_LR} --sampletype SINT16 --rate 44100 --preset N in.raw --output x.ofr`.
Reference decode via `/tmp/ref_gen` (compile `ref_gen.c` with `-arch x86_64 ... libOptimFROG.0.dylib`).
The COMP block's "2 bytes reserved" word encodes `pred=(w>>6)&0x1f, ent=w>>11, post=w&0x3f`.

## Test status (preset matrix, 16-bit, realistic non-tonal sources)

**`pred_type=1` path is 100% complete** — presets 0-3 (fast/normal/high) PASS for BOTH mono and stereo,
across entropy types 1 & 2 and post types 1 & 2. 8/24 preset×channel combos pass.

| Combo | Status |
|---|---|
| pred=1, ent∈{1,2}, post∈{1,2}, mono+stereo (presets 0-3) | PASS bit-exact |
| pred=3 (presets 4-10) | FAIL — not implemented |
| pred=3 + ent=3 (preset max) | FAIL — not implemented |

Remaining pred=1 gap: **post_type=2 value-remap table** (`FUN_00017f00`/`FUN_00018cb0`), only active when the
transform flag `obj+0x9a`==1 — triggered by tonal/synthetic signals (pure sine, ramp), NOT real audio.

## Next work (ordered by cost)

1. **pred_type=3** (presets 4→max) — IN PROGRESS, fully reverse-engineered. **See `doc/pred3_analysis.md`
   for the complete function-by-function map** (object layout, decode loop, cascade NLMS predict/update,
   stage FIR, final combiner, and the init that reads params + decodes the entropy-coded segment schedule).
   It is a DUAL predictor: standard LPC (object+0x10, same as pred=1) + a secondary **cascade of float32
   NLMS adaptive filters** (object+0x42c60), alternating per segment via an entropy-coded schedule (0x437b8).
   All ~15 helper functions decompiled. Main implementation risks: float32 bit-exactness (compile that TU
   WITHOUT -ffast-math, keep the ×8 / 4-accumulator unroll order), and decoding the embedded schedule with
   the existing adaptive-tree context code. This is the single largest remaining component.
2. **entropy_type=3** (preset max, acm).
3. **post_type=2 value-remap table** (only for tonal signals).
4. Encoder (not started).

## Recent fixes (this session)
- entropy path selection by `type==1 && channels==2` (was `is_fast_stereo`) → ent=2 stereo works.
- `OFR_PostProcessor::m_channels` was never set → set it in `init`.
- `data_bits = ofr_bitlen(min,max)` computed for mono too (was hardcoded 13) → entropy depth correct.
- **mono predictor decode** rewritten to match `FUN_00006f80`: clamp prediction to [min,max], apply
  `((clamp+err)<<shift)>>shift`, update with the final value. min/max/shift set from post-processor.
  (The old version was identity — only worked when no clamp/shift was needed, e.g. sine_mono.)

## Workflow rules

- No `git commit` / `git push` without explicit request
- Comments: sparse, English, lowercase inside functions
- Use `rtk ls` instead of `ls`
