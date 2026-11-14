# IEEE Float (`.off`)

Same container as the lossless codec, with `OFRX` as the magic of the main block. Each `COMP` block
is an ordinary integer block (the encoder here uses `pred=1`, `ent=2`, `post=1`, but any type is
valid) followed by a `DETA` sub-block that holds what the integers can't.

## DETA sub-block

```
DETA  size  crc32  range-coded payload (size - 4 bytes)
```

The payload starts with four uniform values, then per-sample data in sample order:

| Field | Range | Meaning |
|---|---|---|
| `exp_range` | 256 | exponent span the integers cover |
| `sig_shift` | 256 | low mantissa bits common to the whole block, dropped from the stream |
| `min_exp + 150` | 512 | exponent of the lowest bit that carries information |
| `max_exp + 150` | 512 | largest exponent in the block |

Unlike the `COMP` range coder, whose first byte is a dummy, this stream must start with `0x40`.

## Reconstruction

Let `v` be the decoded integer of a sample.

**`v != 0`**: `v` is the sign and the top of the mantissa, implicit bit included. With `L` the index
of its highest set bit, the exponent is `L - exp_range + max_exp`. Mantissa bits below those are
read from the DETA stream when `min(23 - sig_shift, exponent - min_exp) > L`.

**`v == 0`**: a symbol from a 279-entry adaptive tree holds `exponent + 150`:

- `0`: a zero, followed by its sign from a 2-symbol adaptive context.
- `0x116`: infinity or NaN, followed by two 12-bit values that hold `mantissa << 1 | sign`.
- otherwise: a sign bit, then the mantissa bits the exponent allows.

## Subnormals

Subnormals are stored like any other value, with a normalized mantissa and an exponent from -149 to
-127, so slot `exponent + 150` runs from 1 to 23. They need the implicit bit shifted back in when
the float is assembled.

This reading follows from the slot layout and hasn't been checked against `off` yet: the
conformance corpus had no subnormals until they were added to
`tests/conformance/gen_synthetic_float.py`. [status.md](status.md) tracks the check.

## Encoder choices

`sig_shift` is always 0 here (no search for common low bits) and `min_exp` is the block's lowest
exponent minus 23, clamped at -150 to fit the header field. The clamp loses nothing, since a
subnormal has fewer significant bits than the distance from its exponent to -150.
