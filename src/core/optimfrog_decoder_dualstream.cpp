// DualStream (.ofs) pred_type=4 decoder: an adaptive, quantized-domain DPCM lossy predictor.
// Reuses OFR_Predictor unmodified for the LPC layer (same weight/interval/order bitstream
// read as pred_type=1 -- FUN_1000165a0 in the off binary is byte-for-byte OFR_Predictor::
// init(rc, bit_depth)) and the existing OFR_RangeCoder/OFR_ModelContext primitives for
// entropy. The only new piece is the quantization-step schedule (second-order-predicted,
// exp-golomb+zigzag coded deltas) and the per-sample DPCM-in-the-quantized-domain formula.
// `--correction` file support (a separate lossless-restore residual channel) is out of scope --
// confirmed via RE that its range-coder object is null for ordinary .ofs files, so that whole
// code path is dead for the common case and correctly absent here.
//
// STATUS (see doc/dualstream_analysis.md / memory): verified bit-exact on clean/tonal mono
// content across quality 0/3/5. NOT yet bit-exact on noisy content -- rare off-by-one-sample
// errors (~0.05% of samples) appear once the quantization step schedule moves away from 1,
// suggesting the `reconstructed = step * idx` formula or the step-schedule indexing has a
// subtle remaining bug not yet root-caused. Do not consider this production-ready yet.
#include "../../include/optimfrog_decoder.h"
#include <cmath>

// same x86 cvtsd2si "integer indefinite" semantics as the rest of the predictor code.
static inline int32_t round_to_int32_cvtsd2si(double x) {
    long lr = std::lrint(x);
    return (lr < INT32_MIN || lr > INT32_MAX) ? INT32_MIN : (int32_t)lr;
}
static inline int32_t shl_wrap(int32_t x, int sh) { return (int32_t)((uint32_t)x << sh); }

void OFR_PredictorDualStreamMono::init(OFR_RangeCoder* rc, int mn, int mx, int dbits, int total) {
    min_val = mn; max_val = mx; shift = 32 - dbits;

    // LPC layer: identical bitstream read to pred_type=1, reused verbatim.
    main_lpc.init(rc, 0);
    lpc_inited = true;

    // quantization-step schedule header + entries.
    uint32_t val = rc->decode_uniform(4096);
    weight_param = val + 1;
    uint32_t count = (val + (uint32_t)total) / weight_param;

    OFR_ModelContext ctx32;
    ctx32.init(32, 0x8000);

    step_schedule.assign(count, 0);
    int32_t prev2 = 1, prev1 = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sym = rc->decode_symbol(ctx32);
        uint32_t width = (sym == 0) ? 1u : sym;
        uint32_t extra = rc->read_uniform_split(width);
        uint32_t raw = (sym == 0) ? extra : ((1u << sym) + extra);
        int32_t delta = (int32_t)(raw >> 1) ^ -(int32_t)(raw & 1);
        int32_t v = (prev1 * 2 - prev2) + delta;
        step_schedule[i] = v;
        prev2 = prev1;
        prev1 = v;
    }

    step_idx = 0;
    current_step = step_schedule.empty() ? 1 : step_schedule[0];
    sample_counter = 0;
}

void OFR_PredictorDualStreamMono::decode(int32_t* dest, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (weight_param != 0 && sample_counter % weight_param == 0 && step_idx < step_schedule.size()) {
            current_step = step_schedule[step_idx++];
        }

        int32_t residual = dest[i];
        double predicted = main_lpc.predict();
        int32_t idx = round_to_int32_cvtsd2si(predicted / (double)current_step) + residual;
        idx = shl_wrap(idx, shift) >> shift;
        int32_t reconstructed = current_step * idx;
        main_lpc.update((double)reconstructed);

        dest[i] = reconstructed;
        sample_counter++;
    }
}
