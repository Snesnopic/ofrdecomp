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
// Mono: verified bit-exact vs the real ofs binary on clean/noisy/real-music content across
// quality 0/1/3/5 (the post1-clamp-must-be-disabled fix, see the pred_type==4 dispatch in
// optimfrog_decoder.cpp).
//
// Stereo: reuses OFR_PredictorStereo_Inner unmodified for the cross-channel LDLT layer (same
// class used by pred_type=1/3 stereo), doubled per-channel quantized-DPCM reconstruction, and
// TWO independent step schedules decoded interleaved from one shared 32-symbol context (RE'd
// from FUN_1000201b0/FUN_1000206b0 in the ofs binary).
#include "../../include/optimfrog_decoder.h"
#include "../../include/optimfrog_tables.h"
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

void OFR_PredictorDualStreamStereo::init(OFR_RangeCoder* rc, int Lmn, int Lmx, int Rmn, int Rmx,
                                          int dbits, int total) {
    min_L = Lmn; max_L = Lmx; min_R = Rmn; max_R = Rmx; shift = 32 - dbits;

    // main cross-channel LPC layer: identical bitstream read to pred_type=1/3 stereo.
    uint32_t w = rc->read_uniform_bits(12);
    main_weight_param = (w == 0xfff) ? (rc->read_uniform_bits(16) + 0x1001) : (w + 2);
    uint32_t iv = rc->read_uniform_bits(3);
    main_interval = (iv == 7) ? (rc->read_uniform_bits(16) + 1) : (uint32_t)DAT_00326238[iv];
    uint32_t od = rc->read_uniform_bits(5);
    if (od == 0x1f) {
        main_max_order = rc->read_uniform_bits(8) + 1;
        main_right_order = rc->read_uniform_bits(8);
    } else {
        main_max_order = (uint32_t)DAT_00326220[od];
        main_right_order = (uint32_t)DAT_00326200[od];
    }
    main.init(((double)main_weight_param - 1.0) / (double)main_weight_param,
              (int)main_max_order, (int)main_max_order - (int)main_right_order, main_interval);

    // step-schedule header + two interleaved schedules (L, R), sharing one 32-symbol context.
    uint32_t val = rc->decode_uniform(4096);
    weight_param = val + 1;
    uint32_t count = ((uint32_t)total / 2 + val) / weight_param;

    OFR_ModelContext ctx32;
    ctx32.init(32, 0x8000);

    step_schedule_L.assign(count, 0);
    step_schedule_R.assign(count, 0);
    int32_t prev2L = 1, prev1L = 1, prev2R = 1, prev1R = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t symL = rc->decode_symbol(ctx32);
        uint32_t widthL = (symL == 0) ? 1u : symL;
        uint32_t extraL = rc->read_uniform_split(widthL);
        uint32_t rawL = (symL == 0) ? extraL : ((1u << symL) + extraL);
        int32_t deltaL = (int32_t)(rawL >> 1) ^ -(int32_t)(rawL & 1);
        int32_t vL = (prev1L * 2 - prev2L) + deltaL;
        step_schedule_L[i] = vL;
        prev2L = prev1L; prev1L = vL;

        uint32_t symR = rc->decode_symbol(ctx32);
        uint32_t widthR = (symR == 0) ? 1u : symR;
        uint32_t extraR = rc->read_uniform_split(widthR);
        uint32_t rawR = (symR == 0) ? extraR : ((1u << symR) + extraR);
        int32_t deltaR = (int32_t)(rawR >> 1) ^ -(int32_t)(rawR & 1);
        int32_t vR = (prev1R * 2 - prev2R) + deltaR;
        step_schedule_R[i] = vR;
        prev2R = prev1R; prev1R = vR;
    }

    step_idx = 0;
    current_step_L = step_schedule_L.empty() ? 1 : step_schedule_L[0];
    current_step_R = step_schedule_R.empty() ? 1 : step_schedule_R[0];
    sample_counter = 0;
}

void OFR_PredictorDualStreamStereo::decode(int32_t* dest, uint32_t count) {
    for (uint32_t i = 0; i < count; i += 2) {
        if (weight_param != 0 && sample_counter % weight_param == 0 && step_idx < step_schedule_L.size()) {
            current_step_L = step_schedule_L[step_idx];
            current_step_R = step_schedule_R[step_idx];
            step_idx++;
        }

        int32_t resL = dest[i];
        double predL = main.predictLeft();
        int32_t idxL = round_to_int32_cvtsd2si(predL / (double)current_step_L) + resL;
        idxL = shl_wrap(idxL, shift) >> shift;
        int32_t reconL = current_step_L * idxL;
        main.updateLeft((double)reconL);
        dest[i] = reconL;

        int32_t resR = dest[i + 1];
        double predR = main.predictRight();
        int32_t idxR = round_to_int32_cvtsd2si(predR / (double)current_step_R) + resR;
        idxR = shl_wrap(idxR, shift) >> shift;
        int32_t reconR = current_step_R * idxR;
        main.updateRight((double)reconR);
        dest[i + 1] = reconR;

        sample_counter++;
    }
}
