# DualStream (`.ofs`, `pred_type=4`)

Lossy codec on the same container, entropy coders and post-processor as the lossless one. Only the
predictor type is new.

## Algorithm

Adaptive DPCM in the quantized domain. It embeds the regular LPC layer unchanged
(`OFR_Predictor` for mono, `OFR_PredictorStereo_Inner` for stereo) and adds a quantization step
that changes along the block:

```
idx           = round(predict() / step) + residual
reconstructed = step * idx
update(reconstructed)
```

The predictor only ever sees dequantized values, so the encoder and the decoder can't drift apart.

## Step schedule

Each block carries a second-order predicted schedule of steps. Deltas are coded with an exp-Golomb
and zigzag scheme, through a 32-symbol adaptive tree. Stereo interleaves two independent schedules
in one shared context.

## Post-processor

A `pred_type=4` block with no correction stream behaves differently from the lossless types:

- after the range reduction, min and max become the full range of the sample type,
  `[-2^(b-1), 2^(b-1) - 1]`, because quantization can legitimately overshoot the source's exact
  range by a fraction of a step;
- identity channels are clamped to that range, which lossless identity blocks never are.

A clamp at the stored min/max would cut those overshoots, which the reference keeps. No clamp at all
would let them wrap on full-scale material.

## Not implemented

`--correction` (the separate residual that restores the original losslessly) and `--ans` (advanced
noise shaping). Their range coder object is null for ordinary files, so that code is dead unless a
file uses them. There is no encoder: a lossy codec has no bit-exact target to reproduce.
