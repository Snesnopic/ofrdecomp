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

void OFR_DecoderEngine::reconstruct_float_block(int32_t* buf, uint32_t count) {
    OFR_BitStream* bs = this->bitstream;

    uint32_t deta_magic = bs->readU32();
    if (deta_magic != 0x41544544) { // "DETA"
        this->has_recoverable_errors = true;
        return;
    }
    uint32_t len = bs->readU32();
    bs->readU32(); // crc32, not verified here
    uint32_t payload_len = (len >= 4) ? (len - 4) : 0;
    uint32_t payload_start = bs->tell();

    bs->range_budget = (long)payload_len;
    OFR_RangeCoder rc;
    rc.init(bs);

    int32_t iVar3 = (int32_t)rc.decode_uniform(256);
    int32_t sigShift = (int32_t)rc.decode_uniform(256);
    int32_t local_60 = (int32_t)rc.decode_uniform(512) - 150;
    int32_t local_74 = (int32_t)rc.decode_uniform(512) - 150;

    OFR_ModelContext ctx2, ctx279;
    ctx2.init(2, 0x8000);
    ctx279.init(0x117, 0x8000);

    for (uint32_t i = 0; i < count; i++) {
        int32_t v = buf[i];
        uint32_t bits;

        if (v != 0) {
            // fast path
            uint32_t mag = (uint32_t)(v < 0 ? -v : v);
            int L = 31;
            while (!((mag >> L) & 1)) L--; // bit length - 1 (mag != 0, guaranteed)
            int32_t exp_i = L - iVar3 + local_74;

            int32_t bits_avail = exp_i - local_60;
            int32_t cap = 23 - sigShift;
            if (cap < bits_avail) bits_avail = cap;
            if (bits_avail < 0) bits_avail = 0;
            int32_t residual_bits = bits_avail - L;

            uint32_t fraction = (mag << (23 - L)) & 0x7fffffu;
            if (residual_bits > 0) {
                uint32_t extra = rc.read_uniform_split((uint32_t)residual_bits);
                fraction |= (extra << (23 - bits_avail));
            }

            uint32_t biased_exp = (uint32_t)(exp_i + 127);
            bits = (biased_exp << 23) | fraction;
            if (v < 0) bits |= 0x80000000u;
        } else {
            // escape path
            uint32_t sym = rc.decode_symbol(ctx279);
            if (sym == 0) {
                // true zero / extreme denormal underflow -> reconstruct as signed zero
                uint32_t signSym = rc.decode_symbol(ctx2);
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

                int32_t bits_avail = exp_i - local_60;
                int32_t cap = 23 - sigShift;
                if (cap < bits_avail) bits_avail = cap;
                if (bits_avail < 0) bits_avail = 0;

                uint32_t fraction = 0;
                if (bits_avail > 0) {
                    fraction = rc.read_uniform_split((uint32_t)bits_avail) << (23 - bits_avail);
                }

                uint32_t biased_exp = (uint32_t)(exp_i + 127);
                bits = (biased_exp << 23) | fraction;
                if (negative) bits |= 0x80000000u;
            }
        }

        buf[i] = (int32_t)bits;
    }

    bs->range_budget = -1;
    uint32_t consumed = bs->tell() - payload_start;
    for (uint32_t k = consumed; k < payload_len; ++k) bs->readU8();
}
