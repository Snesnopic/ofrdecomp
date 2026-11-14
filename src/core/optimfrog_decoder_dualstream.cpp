// DualStream (.ofs) pred_type=4 decoder: an adaptive, quantized-domain DPCM lossy predictor on top
// of the same LPC layers as pred_type=1, plus a per-block quantization-step schedule
// (second-order-predicted, exp-golomb+zigzag coded deltas). `--correction` files (a separate
// lossless-restore residual channel) are not supported, see optimfrog_decoder.h.
#include "../../include/optimfrog_decoder.h"

namespace {

// one schedule entry: a zigzag coded delta against the linear extrapolation of the last two
struct StepDecoder {
    int32_t prev2 = 1, prev1 = 1;
    int32_t next(OFR_RangeCoder* rc, OFR_ModelContext& ctx) {
        uint32_t sym = rc->decode_symbol(ctx);
        uint32_t width = (sym == 0) ? 1u : sym;
        uint32_t extra = rc->read_uniform_split(width);
        uint32_t raw = (sym == 0) ? extra : ((1u << sym) + extra);
        uint32_t delta = (raw >> 1) ^ (0u - (raw & 1u));
        int32_t v = (int32_t)((uint32_t)prev1 * 2u - (uint32_t)prev2 + delta);
        prev2 = prev1;
        prev1 = v;
        return v;
    }
};

// schedule index plus residual, wrapped to the data width, back to a sample
inline int32_t dequantize(double predicted, int32_t step, int32_t residual, int shift) {
    uint32_t idx = (uint32_t)round_to_int32_cvtsd2si(predicted / (double)step) + (uint32_t)residual;
    int32_t wrapped = shl_wrap((int32_t)idx, shift) >> shift;
    return (int32_t)((uint32_t)step * (uint32_t)wrapped);
}

} // namespace

void OFR_PredictorDualStreamMono::init(OFR_RangeCoder* rc, int dbits, uint32_t total) {
    shift = 32 - dbits;
    main_lpc.init(rc);

    uint32_t val = rc->decode_uniform(4096);
    weight_param = val + 1;
    uint64_t count = ((uint64_t)val + total) / weight_param;

    OFR_ModelContext ctx32;
    ctx32.init(32, 0x8000);

    step_schedule.clear();
    StepDecoder steps;
    for (uint64_t i = 0; i < count; i++) step_schedule.push_back(steps.next(rc, ctx32));

    step_idx = 0;
    current_step = step_schedule.empty() ? 1 : step_schedule[0];
    sample_counter = 0;
}

void OFR_PredictorDualStreamMono::decode(int32_t* dest, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (sample_counter % weight_param == 0 && step_idx < step_schedule.size()) {
            current_step = step_schedule[step_idx++];
        }

        int32_t reconstructed = dequantize(main_lpc.predict(), current_step, dest[i], shift);
        main_lpc.update((double)reconstructed);

        dest[i] = reconstructed;
        sample_counter++;
    }
}

void OFR_PredictorDualStreamStereo::init(OFR_RangeCoder* rc, int dbits, uint32_t total) {
    shift = 32 - dbits;

    OFR_StereoLpcParams lpc;
    lpc.read(rc);
    main.init(((double)lpc.weight_param - 1.0) / (double)lpc.weight_param, lpc.max_order,
              lpc.max_order - lpc.right_order, lpc.update_interval);

    uint32_t val = rc->decode_uniform(4096);
    weight_param = val + 1;
    uint64_t count = ((uint64_t)total / 2 + val) / weight_param;

    OFR_ModelContext ctx32;
    ctx32.init(32, 0x8000);

    step_schedule_L.clear();
    step_schedule_R.clear();
    StepDecoder steps_L, steps_R;
    for (uint64_t i = 0; i < count; i++) {
        step_schedule_L.push_back(steps_L.next(rc, ctx32));
        step_schedule_R.push_back(steps_R.next(rc, ctx32));
    }

    step_idx = 0;
    current_step_L = step_schedule_L.empty() ? 1 : step_schedule_L[0];
    current_step_R = step_schedule_R.empty() ? 1 : step_schedule_R[0];
    sample_counter = 0;
}

void OFR_PredictorDualStreamStereo::decode(int32_t* dest, uint32_t count) {
    for (uint32_t i = 0; i < count; i += 2) {
        if (sample_counter % weight_param == 0 && step_idx < step_schedule_L.size()) {
            current_step_L = step_schedule_L[step_idx];
            current_step_R = step_schedule_R[step_idx];
            step_idx++;
        }

        int32_t reconL = dequantize(main.predictLeft(), current_step_L, dest[i], shift);
        main.updateLeft((double)reconL);
        dest[i] = reconL;

        int32_t reconR = dequantize(main.predictRight(), current_step_R, dest[i + 1], shift);
        main.updateRight((double)reconR);
        dest[i + 1] = reconR;

        sample_counter++;
    }
}
