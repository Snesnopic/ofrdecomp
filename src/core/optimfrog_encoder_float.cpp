// IEEE Float (.off, "OFRX") encoder: the dual of optimfrog_decoder_float.cpp's
// reconstruct_float_block. Transforms each float32 sample into an integer for the existing
// lossless pred1/ent2/post1 encoder (reused as-is, unmodified) plus a "DETA" sub-block carrying
// whatever that transform couldn't capture (escaped samples, residual mantissa bits).
//
// Chosen per-block parameters (sigShift is fixed at 0, i.e. no GCD-based bit-shaving
// optimization) don't need to match what the reference encoder would pick -- only be valid and
// self-consistent, same philosophy as the rest of this project's encoder (see
// optimfrog_encoder.cpp). Verified interoperable with the real off binary's decoder.
#include "../../include/optimfrog_encoder.h"
#include <algorithm>
#include <cstring>

struct FloatBlockStats { int32_t minExp, maxExp; };

// per-block signed-exponent min/max, excluding zero and inf/nan samples. subnormals count with
// their normalized exponent, down to -149.
static FloatBlockStats compute_block_stats(const float* samples, uint32_t n) {
    int32_t minExp = 0x7fffffff, maxExp = -0x7fffffff - 1;
    bool any = false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bits; memcpy(&bits, &samples[i], 4);
        uint32_t biased = (bits >> 23) & 0xff;
        uint32_t mant = bits & 0x7fffffu;
        if (biased == 0xff) continue; // inf/nan
        int32_t e;
        if (biased == 0) {
            if (mant == 0) continue; // true zero
            uint32_t m = mant; e = -126;
            while (!(m & 0x800000u)) { m <<= 1; e--; }
        } else {
            e = (int32_t)biased - 127;
        }
        if (e < minExp) minExp = e;
        if (e > maxExp) maxExp = e;
        any = true;
    }
    if (!any) { minExp = 0; maxExp = 0; }
    return {minExp, maxExp};
}

// dual of the decoder's per-sample reconstruction: writes escape/residual data into deta_enc,
// returns the OUTPUT integer fed to the ordinary pred1/ent2/post1 encoder.
static int32_t float_to_output(float f, int32_t iVar3, int32_t local_60, int32_t local_74, int32_t sigShift,
                                OFR_RangeEncoder& deta_enc, OFR_ModelContext& ctx2, OFR_ModelContext& ctx279) {
    uint32_t bits; memcpy(&bits, &f, 4);
    uint32_t biased_exp = (bits >> 23) & 0xff;
    uint32_t mant = bits & 0x7fffffu;
    bool sign = (bits >> 31) != 0;

    if (biased_exp == 0xff) { // inf/nan
        deta_enc.encode_symbol(ctx279, 0x116);
        uint32_t packed = (mant << 1) | (sign ? 1u : 0u);
        deta_enc.encode_bits(12, packed & 0xfffu);
        deta_enc.encode_bits(12, packed >> 12);
        return 0;
    }

    int32_t exp_i = 0;
    uint32_t frac = 0;
    bool is_true_zero = false;

    if (biased_exp == 0) {
        if (mant == 0) {
            is_true_zero = true;
        } else {
            uint32_t m = mant; int32_t e = -126;
            while (!(m & 0x800000u)) { m <<= 1; e--; }
            exp_i = e;
            frac = m & 0x7fffffu;
        }
    } else {
        exp_i = (int32_t)biased_exp - 127;
        frac = mant;
    }

    if (is_true_zero) {
        deta_enc.encode_symbol(ctx279, 0);
        deta_enc.encode_symbol(ctx2, sign ? 1u : 0u);
        return 0;
    }

    int32_t iVar5 = exp_i + iVar3 - local_74;
    int32_t bits_avail = exp_i - local_60;
    int32_t cap = 23 - sigShift;
    if (cap < bits_avail) bits_avail = cap;
    if (bits_avail < 0) bits_avail = 0;

    if (iVar5 >= 0) {
        // fast path
        uint32_t mantissa_full = 0x800000u | frac;
        int32_t mag = (int32_t)(mantissa_full >> (23 - iVar5));
        int32_t out = sign ? -mag : mag;
        int32_t residual_bits = bits_avail - iVar5;
        if (residual_bits > 0) {
            uint32_t extra = (frac >> (23 - bits_avail)) & ((1u << residual_bits) - 1u);
            deta_enc.encode_split((uint32_t)residual_bits, extra);
        }
        return out;
    } else {
        // escape
        deta_enc.encode_symbol(ctx279, (uint32_t)(exp_i + 150));
        deta_enc.encode_uniform(2, sign ? 1u : 0u);
        if (bits_avail > 0) {
            uint32_t topbits = frac >> (23 - bits_avail);
            deta_enc.encode_split((uint32_t)bits_avail, topbits);
        }
        return 0;
    }
}

// Encodes one block of n interleaved values into the DETA byte stream + the OUTPUT integer array
// fed to the lossless encoder. The DETA header and contexts aren't per-channel, matching how the
// decoder processes the interleaved array uniformly.
static void build_deta_and_output(const float* samples, uint32_t n, std::vector<uint8_t>& deta,
                                   std::vector<int32_t>& output) {
    FloatBlockStats st = compute_block_stats(samples, n);
    int32_t sigShift = 0;
    int32_t iVar3 = std::min(23 - sigShift, st.maxExp - st.minExp);
    int32_t local_60 = st.minExp - (23 - sigShift);
    if (local_60 < -150) local_60 = -150; // stay within the 9-bit (val+150 in [0,511]) header field range
    int32_t local_74 = st.maxExp;

    OFR_RangeEncoder deta_enc;
    deta_enc.encode_uniform(256, (uint32_t)iVar3);
    deta_enc.encode_uniform(256, (uint32_t)sigShift);
    deta_enc.encode_uniform(512, (uint32_t)(local_60 + 150));
    deta_enc.encode_uniform(512, (uint32_t)(local_74 + 150));

    OFR_ModelContext ctx2, ctx279;
    ctx2.init(2, 0x8000);
    ctx279.init(0x117, 0x8000);

    output.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        output[i] = float_to_output(samples[i], iVar3, local_60, local_74, sigShift, deta_enc, ctx2, ctx279);
    }
    deta_enc.flush();
    // DETA's range coder init (FUN_10002bef0/FUN_10002b5a0 in off) writes/checks an explicit
    // '@' (0x40) marker byte as the stream's first byte -- unlike the main COMP range coder,
    // where the first emitted byte really is an arbitrary, ignored dummy. Force it here.
    deta_enc.out[0] = 0x40;

    deta.clear();
    ofr_put32(deta, OFR_TAG_DETA);
    ofr_put32(deta, (uint32_t)deta_enc.out.size() + 4);
    ofr_put32(deta, ofr_crc32(deta_enc.out.data(), deta_enc.out.size()));
    deta.insert(deta.end(), deta_enc.out.begin(), deta_enc.out.end());
}

bool ofr_encode_float(const float* samples, uint64_t frames, int channels, uint32_t samplerate,
                      const OFR_EncodeParams& params, std::vector<uint8_t>& file,
                      const std::vector<uint8_t>& head, const std::vector<uint8_t>& tail) {
    if (channels != 1 && channels != 2) return false;

    const uint32_t per_block = ofr_effective_block_frames(params, frames, channels);
    const size_t block_count = frames ? (size_t)((frames + per_block - 1) / per_block) : 1;
    std::vector<OFR_EncodedBlock> blocks(block_count);

    ofr_parallel_for(block_count, params.threads, [&](size_t i) {
        uint64_t first = (uint64_t)i * per_block;
        uint32_t n = (uint32_t)std::min<uint64_t>(per_block, frames - first);

        std::vector<uint8_t> deta;
        std::vector<int32_t> output;
        build_deta_and_output(samples + first * (uint64_t)channels, n * (uint32_t)channels, deta, output);

        blocks[i] = ofr_encode_comp_block(output.data(), n, channels, 32, OFR_SAMPLE_FLOAT32, params);
        blocks[i].deta = std::move(deta);
    });

    ofr_assemble_file(true, frames * (uint64_t)channels, OFR_SAMPLE_FLOAT32, channels, samplerate, blocks, file, head, tail);
    return true;
}
