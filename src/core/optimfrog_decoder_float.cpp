// IEEE Float (.off, "OFRX") support: reconstructs original float32 bit patterns from the
// per-block integer array decoded by the ordinary pred1/ent2/post1 pipeline, using the "DETA"
// sub-block that immediately follows each COMP block on disk.
//
// Per-sample encoding (see doc -- reverse engineered from the off/ofs binaries):
//   - integer != 0 ("fast path"): the integer IS the shifted 24-bit mantissa (with implicit bit),
//     sign-carrying. exponent is recovered from the integer's own bit-length; residual low
//     mantissa bits (if any) follow as raw range-coded bits.
//   - integer == 0 ("escape"): exponent read via a 279-symbol adaptive tree (slot = exponent+150;
//     slot 0 doubles as the true-zero/extreme-denormal sentinel, slot 0x116 as inf/nan), sign and
//     mantissa read separately (raw bits for the normal case, a 2-symbol adaptive context for the
//     zero-sentinel's sign, two raw 12-bit chunks for inf/nan's sign+mantissa).
#include "../../include/optimfrog_decoder.h"

// exponents below the normal range are subnormals, still carried with a normalized mantissa:
// the implicit bit has to be shifted back into the stored fraction.
static uint32_t assemble_float(int32_t exp_i, uint32_t fraction) {
    if (exp_i >= -126) return ((uint32_t)(exp_i + 127) << 23) | fraction;
    int32_t shift = -126 - exp_i;
    return shift < 24 ? (0x800000u | fraction) >> shift : 0u;
}

void OFR_FloatBlock::init(std::vector<uint8_t>&& payload) {
    payload_ = std::move(payload);
    rc_.init(payload_.data(), payload_.size());

    exp_range_ = (int32_t)rc_.decode_uniform(256);
    sig_shift_ = (int32_t)rc_.decode_uniform(256);
    min_exp_ = (int32_t)rc_.decode_uniform(512) - 150;
    max_exp_ = (int32_t)rc_.decode_uniform(512) - 150;

    ctx2_.init(2, 0x8000);
    ctx279_.init(0x117, 0x8000);
}

void OFR_FloatBlock::reconstruct(int32_t* buf, uint32_t count) {
    OFR_RangeCoder& rc = rc_;
    const int32_t cap = 23 - sig_shift_;

    for (uint32_t i = 0; i < count; i++) {
        int32_t v = buf[i];
        uint32_t bits;

        if (v != 0) {
            // fast path
            uint32_t mag = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
            int L = 31;
            while (!((mag >> L) & 1)) L--; // bit length - 1 (mag != 0, guaranteed)
            if (L > 23) throw OFR_StreamError("float mantissa too wide");
            int32_t exp_i = L - exp_range_ + max_exp_;

            int32_t bits_avail = exp_i - min_exp_;
            if (cap < bits_avail) bits_avail = cap;
            if (bits_avail < 0) bits_avail = 0;
            int32_t residual_bits = bits_avail - L;

            uint32_t fraction = (mag << (23 - L)) & 0x7fffffu;
            if (residual_bits > 0) {
                uint32_t extra = rc.read_uniform_split((uint32_t)residual_bits);
                fraction |= (extra << (23 - bits_avail));
            }

            bits = assemble_float(exp_i, fraction);
            if (v < 0) bits |= 0x80000000u;
        } else {
            // escape path
            uint32_t sym = rc.decode_symbol(ctx279_);
            if (sym == 0) {
                // true zero / extreme denormal underflow -> reconstruct as signed zero
                uint32_t signSym = rc.decode_symbol(ctx2_);
                bits = signSym ? 0x80000000u : 0u;
            } else if (sym == 0x116) {
                // inf/nan
                uint32_t lo = rc.read_uniform_bits(12);
                uint32_t hi = rc.read_uniform_bits(12);
                uint32_t packed = lo | (hi << 12);
                bool negative = (packed & 1) != 0;
                uint32_t mantissa = packed >> 1;
                bits = 0x7f800000u | mantissa;
                if (negative) bits |= 0x80000000u;
            } else {
                int32_t exp_i = (int32_t)sym - 150;
                bool negative = rc.decode_uniform(2) != 0;

                int32_t bits_avail = exp_i - min_exp_;
                if (cap < bits_avail) bits_avail = cap;
                if (bits_avail < 0) bits_avail = 0;

                uint32_t fraction = 0;
                if (bits_avail > 0) {
                    fraction = rc.read_uniform_split((uint32_t)bits_avail) << (23 - bits_avail);
                }

                bits = assemble_float(exp_i, fraction);
                if (negative) bits |= 0x80000000u;
            }
        }

        buf[i] = (int32_t)bits;
    }
}
