# Status

## How things get verified

There are three levels of evidence, and they aren't interchangeable:

- **Reference**: `tests/conformance/` compares against the official binaries in both directions.
  This is the only proof of compatibility. It runs in CI, where the binaries are downloaded.
- **Golden**: output compared byte for byte with the previous reference-verified build. Proves
  that a change altered nothing, for the paths the matrix covers.
- **Local**: `ctest`, sanitizers and fuzzing. These prove self-consistency and robustness, not
  compatibility: an encoder and a decoder can agree with each other and both be wrong.

The rewrite after commit `46c3868` (the last full CI run) was checked with **Golden** (1476 encode
and decode combinations, no byte differs from `46c3868`), with **Local**, and on 2026-11-14 with
**Reference** against the official library (`libOptimFROG.0.dylib` under Rosetta): direction B of
`run.sh` and `sdk_diff.sh`. Direction A and the Float and DualStream suites need the official
`ofr`/`off`/`ofs` tools, which weren't available locally; CI runs them.

## Coverage

| Area | Evidence |
|---|---|
| Lossless decoder, `pred` 1 and 3, `ent` 1 to 3, `post` 1 and 2, mono and stereo, 8/16/24/32-bit | Reference (CI at `46c3868`), Golden since |
| Lossless encoder, same matrix | Reference |
| `pred_type=2`, mono and stereo, `ent` 1 and 3 | Reference, direction B. No file from the official encoder is known to use it |
| Encoder with several blocks, types varying per block | Reference, direction B, including 1.5M-frame signals |
| Encoder orders: stereo up to 128, mono up to 127 | Reference, direction B. See below for mono 128 |
| C API: return values, `readHead`/`readTail`, seek, `decodeFile`, `infoFile`, tags, custom readers | Reference, `sdk_diff.sh` |
| Damaged and truncated files | Reference: `sdk_files.py` patterns, and 559 of 560 random single-bit damages (see [format.md](format.md)); fuzzed |
| Float decoder and encoder, ordinary values, zeros | Reference (CI at `46c3868`), Golden since |
| Float subnormals, infinities, NaNs | Local. *Pending*, needs `off` |
| DualStream decoder, mono and stereo | Reference, by hand; no script |
| Full-scale DualStream files (post-processor clamp) | Read from the disassembly. *Pending*, needs `ofs` |

The Linux reference library decodes some `pred=3` streams differently from the macOS one, so the
Linux CI job leaves cascade predictors out. See the comment in `.github/workflows/conformance.yml`.

## Encoder

Parameters go through `OFR_EncodeParams`; nothing is read from the environment by the library
(`ofrenc` maps its `OFR_*` variables onto the struct). Long files are split into independent blocks,
which are encoded in parallel when `threads` allows it and searched separately when `search` is on.
The search only tries configurations that existed before the rewrite; `pred=2` and the higher
orders now have reference evidence too and could be added, which would need its own size
comparison first.

Predictor orders stop at 128: the reference keeps its covariance matrices in 128-row arrays, so
the 192 and 256 entries of its order table can't work. That explains the desync seen long ago at
those orders. Mono order 128 itself is legal, and decoded files using it are accepted, but the
official decoder throws away such blocks for some signals (a 32-bit sine does it) while 127 always
worked. The encoder therefore stops at 127 in mono.

## Not implemented

- RECV (recovery) blocks: no known way to get a reference file that contains real RECV data.
- DualStream `--correction` and `--ans`: dead code for ordinary files. Also no DualStream encoder.
- Files from before the 4.5 series: the official library can't open them either.
- Skipping the LPC solve during cascade segments in `pred=3`, which the reference does for speed.
