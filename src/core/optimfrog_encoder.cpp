// lossless encoder. reuses the decoder's predictors forward, and pairs them with the duals of the
// entropy decoders; all it adds is the range encoder and the container writer.
#include "../../include/optimfrog_decoder.h"
#include "../../include/optimfrog_encoder.h"
#include "../../include/optimfrog_tables.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// ---- fast entropy encoder (dual of OFR_EntropyDecoder fast path) ----
struct FastCtxE {
    std::vector<uint32_t> freqs;
    uint32_t num_symbols = 0, num_nodes = 0, total_freq = 0, limit = 0;
};
static uint32_t fast_rebuild(std::vector<uint32_t>& f, uint32_t node, uint32_t nn) {
    if (node < nn) { uint32_t l = fast_rebuild(f, node*2, nn), r = fast_rebuild(f, node*2+1, nn); f[node] = l; return l + r; }
    return f[node];
}
static void fast_ctx_init(FastCtxE& c, uint32_t ns, uint32_t limit) {
    c.num_symbols = ns; uint32_t nn = 1; while (nn < ns) nn <<= 1;
    c.num_nodes = nn; c.limit = limit; c.freqs.assign(nn*2, 0u);
    for (uint32_t i = 0; i < ns; i++) c.freqs[nn+i] = 1u;
    c.total_freq = ns; fast_rebuild(c.freqs, 1, nn);
}
static void fast_ctx_halve(FastCtxE& c) {
    c.total_freq = 0;
    for (uint32_t i = 0; i < c.num_symbols; i++) { uint32_t& f = c.freqs[c.num_nodes+i]; f = ((f-1u)>>1)+1u; c.total_freq += f; }
    fast_rebuild(c.freqs, 1, c.num_nodes);
}

struct FastEntropyEncoder {
    std::vector<FastCtxE> ctx;   // shared across channels (as in the decoder)
    double weight = 0, weight2 = 0;
    void init(uint32_t bit_depth, uint32_t w) {
        weight = (double)(w - 1) / (double)w;
        weight2 = 1.0 - weight;   // as in the decoder: for a w that isn't a power of two, 1/w differs
        uint32_t num_contexts = bit_depth * 2u;
        uint32_t per = (bit_depth < 4u) ? (1u << bit_depth) : (bit_depth * 8u - 0x10u);
        ctx.assign(num_contexts, FastCtxE());
        for (uint32_t i = 0; i < num_contexts; i++) fast_ctx_init(ctx[i], per, 0x8000u);
    }
    void encode_sample(OFR_RangeEncoder& enc, double& var, int32_t residual) {
        uint32_t value = (residual >= 0) ? ((uint32_t)residual << 1) : (((0u - (uint32_t)residual) << 1) - 1u);
        uint64_t vbits; std::memcpy(&vbits, &var, 8);
        uint32_t idx = (uint32_t)((vbits >> 52) + 0xfffffc01u);
        FastCtxE& c = ctx[idx];

        uint32_t symbol, group = 0, extra = 0;
        if (value < 8u) { symbol = value; }
        else {
            uint32_t lg = (uint32_t)std::bit_width(value) - 1u;   // floor(log2(value))
            group = lg - 3u;
            uint32_t r = value - (1u << (group + 3u));
            uint32_t j = r >> group;
            extra = r & ((1u << group) - 1u);
            symbol = 8u + (group << 3) + j;
        }

        // encode symbol via the same tree walk + increments as fast_decode_sample
        uint32_t step = enc.range / c.total_freq;
        auto& f = c.freqs;
        int L = 0; while ((1u << L) < c.num_nodes) L++;
        uint32_t target = c.num_nodes + symbol;
        uint32_t node = 1, acc = 0;
        for (int i = L - 1; i >= 0; i--) {
            if (((target >> i) & 1u) == 0) { f[node] += 2u; node = 2*node; }
            else { acc += f[node]; node = 2*node + 1; }
        }
        uint32_t leaf_freq = f[node];
        enc.low += (uint64_t)acc * step;
        enc.range = (acc + leaf_freq < c.total_freq) ? (step * leaf_freq) : (enc.range - acc * step);
        enc.renorm();
        f[node] += 2u;
        c.total_freq += 2u;
        if (c.limit <= c.total_freq) fast_ctx_halve(c);

        if (symbol >= 8u) enc.encode_bits(group, extra);

        var = (double)value * (double)value * weight2 + weight2 + var * weight;
    }
};

// ---- slow entropy encoder (dual of OFR_EntropyDecoder::decode_one_sample, FUN_00109ab0) ----
static inline int floor_log2_u(uint32_t x) { return std::bit_width(x) - 1; }
struct SlowEntropyEncoder {
    OFR_ModelContext master;   // 32-symbol tree (leaves == num_nodes == 32), increment 2
    double weight = 0, weight2 = 0;
    uint32_t bit_depth = 0;
    void init(uint32_t bd, uint32_t w) {
        weight = (double)(w - 1) / (double)w;
        weight2 = 1.0 - weight;   // matches decoder's derivation (1-ULP off vs 1/w); see decoder fix
        bit_depth = bd;
        master.init(32u, 0x8000u);
    }
    void encode_sample(OFR_RangeEncoder& enc, double& var, int32_t residual) {
        uint32_t raw_val = (residual >= 0) ? ((uint32_t)residual << 1) : (((0u - (uint32_t)residual) << 1) - 1u);
        uint32_t uVar6 = ((uint32_t)(int64_t)var >> 2) & 0x3fffffffu;
        uint32_t uVar16 = uVar6 + 1u;
        bool escape = false;
        if (uVar16 < 0x4001u) {
            uint32_t q = raw_val / uVar16;
            if (q < 31u) {
                enc.encode_symbol(master, q);
                enc.encode_uniform(uVar16, raw_val - q * uVar16);
            } else {
                enc.encode_symbol(master, 31u);
                uVar16 = uVar16 * 31u; escape = true;
            }
        } else {
            int iVar14 = floor_log2_u(uVar16);
            uint32_t sym2 = raw_val >> iVar14;
            if (sym2 < 31u) {
                enc.encode_symbol(master, sym2);
                enc.encode_bits((uint32_t)iVar14, raw_val & ((1u << iVar14) - 1u));
            } else {
                enc.encode_symbol(master, 31u);
                uVar16 = 0x1fu << iVar14; escape = true;
            }
        }
        if (escape) {
            int new_bits = floor_log2_u(uVar16);
            uint32_t remaining = bit_depth - (uint32_t)new_bits;
            int total_bits = floor_log2_u(raw_val);
            enc.encode_uniform(remaining, (uint32_t)(total_bits - new_bits));
            enc.encode_bits((uint32_t)total_bits, raw_val - (1u << total_bits));
        }
        // no clamp: raw_val is always treated as non-negative, matching the decoder fix
        var = var * weight + (double)(uint64_t)raw_val * weight2;
    }
};

// ---- ent=3 (acm) encoder (dual of OFR_EntropyAcm). all integer. ----
static uint32_t acm_mag_exp(int64_t mag) {
    uint32_t u17 = (uint32_t)((uint64_t)mag >> 16);
    uint32_t u19 = (uint32_t)((uint64_t)mag >> 32) & 0xffffu;
    if (u17 < 0x10000u) u19 = u17;
    uint32_t b = (u17 > 0xffffu) ? 16u : 0u;
    if (u19 > 0xffu) { u19 >>= 8; b |= 8u; }
    if (u19 > 0xfu)  { u19 >>= 4; b += 4u; }
    if (u19 > 3u)    { u19 >>= 2; b += 2u; }
    return (u19 > 1u ? 1u : 0u) + b;
}
static uint32_t acm_int_bitlen(uint32_t v) {
    uint32_t u19 = (v >> 16) ? (v >> 16) : v;
    uint32_t b = (v > 0xffffu) ? 16u : 0u;
    if (u19 > 0xffu) { u19 >>= 8; b |= 8u; }
    if (u19 > 0xfu)  { u19 >>= 4; b += 4u; }
    if (u19 > 3u)    { u19 >>= 2; b += 2u; }
    return (u19 > 1u ? 1u : 0u) + b;
}
struct AcmEncoder {
    struct Chan {
        uint32_t reset = 64, state = 0, countdown = 0;
        int64_t hist[8] = {0,0,0,0,0,0,0,0};
        std::vector<OFR_ModelContext> bitlen;   // 9 contexts
        OFR_ModelContext vctx;                  // single shared value context
        uint32_t k = 0, nsym = 0;
    };
    std::vector<Chan> chans;
    uint32_t bit_depth = 0;

    void init(OFR_RangeEncoder& enc, uint32_t bd, uint32_t ch) {
        bit_depth = bd;
        chans.assign(ch, Chan());
        const uint32_t T_SCALE_S = 3, T_K = 256, T_NSYM = 32;   // scale=1<<15, k, nsym
        for (uint32_t ci = 0; ci < ch; ++ci) {
            Chan& c = chans[ci];
            enc.encode_uniform(2, 0);            // msb_flag = 0
            enc.encode_uniform(16, 0);           // reset idx 0 -> reset = 1<<6 = 64
            c.reset = 64;
            enc.encode_uniform(2, 1);            // template present
            enc.encode_uniform(2, 0);            // t_transform = 1
            enc.encode_uniform(8, T_SCALE_S);    // t_scale = 1<<(3+12) = 0x8000
            enc.encode_bits(8, T_K - 1);         // t_k = 256
            enc.encode_uniform(128, T_NSYM - 2); // t_nsym = 32
            // per-exponent context map: one context (cur=0) for all exponents
            for (uint32_t e = 0; e < bit_depth; ++e)
                enc.encode_uniform(2, e == 0 ? 1 : 0);
            c.k = T_K; c.nsym = T_NSYM;
            c.vctx.init(T_NSYM, 0x8000u);
            c.bitlen.assign(9, OFR_ModelContext());
            for (int i = 0; i < 9; ++i) c.bitlen[i].init(9, 0x8000u);
            for (int i = 0; i < 8; ++i) c.hist[i] = 0;
            c.countdown = 0; c.state = 0;
        }
    }

    void encode_sample(OFR_RangeEncoder& enc, Chan& c, uint32_t value) {
        uint32_t state = c.state;
        if (c.countdown == 0) {
            enc.encode_symbol(c.bitlen[state], 0);   // chosen state symbol = 0 (never 8)
            c.state = 0; c.countdown = c.reset - 1u; state = 0;
        } else {
            c.countdown--;
        }
        int64_t mag = c.hist[state];                 // msb_flag=0, state=0
        uint32_t e = acm_mag_exp(mag);
        (void)e;                                     // vmap[e] == 0 for all e
        uint32_t k = c.k;
        uint32_t pred = (uint32_t)(((uint64_t)k * (uint64_t)mag) >> 24) + 1u;
        uint32_t sym2 = value / pred;
        if (sym2 <= c.nsym - 2u) {
            enc.encode_symbol(c.vctx, sym2);
            uint32_t residual = value - sym2 * pred;
            if (pred < 0x4001u) {
                enc.encode_uniform(pred, residual);
            } else {
                uint32_t u19 = (pred - 1u + 0x4000u) >> 14;
                uint32_t u18 = (pred - 1u + u19) / u19;
                enc.encode_uniform(u18, residual / u19);
                enc.encode_uniform(u19, residual % u19);
            }
        } else {
            enc.encode_symbol(c.vctx, c.nsym - 1u);  // escape
            uint32_t v = (c.nsym - 1u) * pred;
            uint32_t bits = acm_int_bitlen(v);
            uint32_t rem = bit_depth - bits;
            uint32_t total_bits = acm_int_bitlen(value);
            enc.encode_uniform(rem, total_bits - bits);
            enc.encode_bits(total_bits, value - (1u << total_bits));
        }
        int64_t lv = (int64_t)value << 16;
        for (int i = 0; i < 8; ++i) c.hist[i] += (lv - c.hist[i]) >> (3 + i);
    }
};

void ofr_put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)v); o.push_back((uint8_t)(v>>8)); o.push_back((uint8_t)(v>>16)); o.push_back((uint8_t)(v>>24));
}
static void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v>>8)); }
static void put_tag(std::vector<uint8_t>& o, uint32_t tag) { ofr_put32(o, tag); }

static const int IVT[8] = { 32, 64, 128, 256, 512, 1024, 2048, 0 };
static const int ODT[9] = { 12, 24, 36, 48, 64, 96, 128, 192, 256 };
static const int CASC_OT[8] = { 512, 256, 128, 64, 128, 64, 32, 16 };
static const int CASC_S2[8] = { 128, 64, 32, 16, 0, 0, 0, 0 };

// cascade settings written by every pred=2/3 block
namespace {
struct CascadeConfig {
    static constexpr uint32_t MAIN_W = 256, FC_W = 256, K = 4, GOLOMB = 0x40;
    static constexpr int N_STAGES = 2;
    std::vector<int> stage_idx;
    std::vector<int> stage_mu10 = { 400, 600 };

    void write(OFR_RangeEncoder& enc) const {
        enc.encode_bits(3, (uint32_t)(N_STAGES - 1));
        enc.encode_bits(1, 0);               // decay flag -> 1.0
        enc.encode_bits(12, FC_W - 2);
        enc.encode_bits(3, K);
        enc.encode_bits(1, 0);               // golomb flag -> 0x40
        for (int s = 0; s < N_STAGES; ++s) {
            enc.encode_bits(5, (uint32_t)stage_idx[s]);
            enc.encode_bits(10, (uint32_t)stage_mu10[s]);
        }
    }
};
} // namespace

static void encode_entropy(OFR_RangeEncoder& enc, const OFR_EncodeParams& p, int channels, int data_bits,
                           const std::vector<int32_t>& res) {
    const uint32_t count = (uint32_t)res.size();
    if (p.ent == 3) {
        AcmEncoder acm; acm.init(enc, (uint32_t)data_bits, (uint32_t)channels);
        for (uint32_t i = 0; i < count; i++) {
            int32_t r = res[i];
            uint32_t value = (r >= 0) ? ((uint32_t)r << 1) : (((0u - (uint32_t)r) << 1) - 1u);
            acm.encode_sample(enc, acm.chans[channels == 2 ? (i & 1) : 0], value);
        }
        return;
    }
    enc.encode_uniform(4096, p.ent_weight - 2);
    if (p.ent == 2) {
        SlowEntropyEncoder ent; ent.init((uint32_t)data_bits, p.ent_weight);
        double var[2] = {0.0, 0.0};   // slow path variance starts at 0.0
        for (uint32_t i = 0; i < count; i++) ent.encode_sample(enc, var[channels == 2 ? (i & 1) : 0], res[i]);
    } else {
        FastEntropyEncoder ent; ent.init((uint32_t)data_bits, p.ent_weight);   // contexts shared by the channels
        double var[2] = {1.0, 1.0};
        for (uint32_t i = 0; i < count; i++) ent.encode_sample(enc, var[channels == 2 ? (i & 1) : 0], res[i]);
    }
}

static inline int residual_of(int sample, int prediction, int mn, int mx, int sh) {
    int cp = std::max(mn, std::min(prediction, mx));
    return ofr_wrap_add(sample, (int32_t)(0u - (uint32_t)cp), sh);
}

// post=1 identity, then the predictor and its forward pass, then the entropy coder
static void encode_mono_payload(OFR_RangeEncoder& enc, const int32_t* samples, uint32_t n, int bps,
                                const OFR_EncodeParams& p) {
    int mn = 0x7fffffff, mx = -0x7fffffff - 1;
    for (uint32_t i = 0; i < n; i++) { int v = samples[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    if (n == 0) { mn = 0; mx = 0; }
    int data_bits = ofr_bitlen(mn, mx);
    int sh = (32 - data_bits) & 0x1f;
    uint32_t bmask = (bps >= 32) ? 0xffffffffu : ((1u << bps) - 1u);

    enc.encode_split((uint32_t)bps, (uint32_t)mn & bmask);
    enc.encode_split((uint32_t)bps, (uint32_t)mx & bmask);
    enc.encode_bits(1, 0);

    std::vector<int32_t> res(n);

    if (p.pred == 2 || p.pred == 3) {
        const bool cascade_only = p.pred == 2;
        int main_iv_idx = 0;                 // interval 32
        int main_od_idx = 1;                 // order 24
        int MAIN_IV = IVT[main_iv_idx], MAIN_OD = ODT[main_od_idx];
        CascadeConfig cfg;
        cfg.stage_idx = { 1, 6 };            // stage order index -> CASC_OT: 1=256, 6=32
        std::vector<int> st_order = { CASC_OT[cfg.stage_idx[0]], CASC_OT[cfg.stage_idx[1]] };

        std::vector<uint8_t> sched(2, 1);
        if (!cascade_only) {
            enc.encode_bits(12, CascadeConfig::MAIN_W - 2);
            enc.encode_bits(3, (uint32_t)main_iv_idx);
            enc.encode_bits(5, (uint32_t)main_od_idx);
        }
        cfg.write(enc);
        if (!cascade_only) {
            // schedule (all-cascade); length nseg+2 with sentinel
            int order_p1 = (int)MAIN_OD + 1;
            int firstcd = std::min(order_p1, (int)n);
            int seg_len = MAIN_IV;
            int nseg = ((int)n + (~firstcd) + seg_len) / seg_len;
            if (nseg < 0) nseg = 0;
            sched.assign((size_t)nseg + 2, 1);
            OFR_ModelContext sctx0, sctx1; sctx0.init(2, 0x8000); sctx1.init(2, 0x8000);
            OFR_ModelContext* sctx[2] = { &sctx0, &sctx1 };
            enc.encode_symbol(*sctx[0], sched[0]);
            uint32_t prev = sched[0];
            for (int i = 1; i <= nseg; ++i) { enc.encode_symbol(*sctx[prev], sched[i]); prev = sched[i]; }
        }

        auto cm = std::make_unique<OFR_PredictorCascadeMono>();
        cm->setup_for_encode(mn, mx, data_bits, (int)n, CascadeConfig::MAIN_W, MAIN_IV, MAIN_OD, CascadeConfig::N_STAGES,
                             1.0, CascadeConfig::FC_W, CascadeConfig::K, CascadeConfig::GOLOMB, st_order, cfg.stage_mu10, sched);
        cm->cascade_only = cascade_only;
        cm->cascade_init(); cm->need_init = false;
        for (uint32_t i = 0; i < n; i++) {
            int out = samples[i];
            if (cascade_only) {
                res[i] = residual_of(out, cm->cascade_predict(), mn, mx, sh);
                cm->cascade_update((double)out);
                continue;
            }
            if (cm->cc_mode == 0) {
                res[i] = residual_of(out, round_to_int32_cvtsd2si(cm->main_lpc.predict()), mn, mx, sh);
                cm->main_lpc.update((double)out); cm->cascade_predict(); cm->cascade_update((double)out);
            } else {
                res[i] = residual_of(out, cm->cascade_predict(), mn, mx, sh);
                cm->cascade_update((double)out); cm->main_lpc.update((double)out);
            }
            if (--cm->cc_count == 0) { cm->cc_mode = cm->schedule[cm->sched_idx]; cm->cc_count = cm->seg_len; cm->sched_idx++; }
        }
    } else {
        enc.encode_uniform(4096, p.pred_weight - 2);
        int iv_idx = -1; for (int i = 0; i < 7; i++) if (IVT[i] == p.mono_interval) iv_idx = i;
        if (iv_idx >= 0) enc.encode_bits(3, (uint32_t)iv_idx);
        else { enc.encode_bits(3, 7); enc.encode_split(16, (uint32_t)(p.mono_interval - 1)); }
        int od_idx = -1; for (int i = 0; i < 9; i++) if (ODT[i] == p.mono_order) od_idx = i;
        if (od_idx >= 0) enc.encode_bits(5, (uint32_t)od_idx);
        else { enc.encode_bits(5, 31); enc.encode_bits(8, (uint32_t)(p.mono_order - 1)); }

        OFR_Predictor pd;
        pd.init(p.mono_order, p.mono_interval, (double)(p.pred_weight - 1) / (double)p.pred_weight);
        pd.min_val = mn; pd.max_val = mx; pd.shift = sh;
        for (uint32_t i = 0; i < n; i++) {
            int v = samples[i];
            res[i] = residual_of(v, round_to_int32_cvtsd2si(pd.predict()), mn, mx, sh);
            pd.update((double)v);
        }
    }

    encode_entropy(enc, p, 1, data_bits, res);
    enc.flush();
}

// interleaved L/R
static void encode_stereo_payload(OFR_RangeEncoder& enc, const int32_t* samples, uint32_t frames, int bps,
                                  const OFR_EncodeParams& p) {
    int mnL = 0x7fffffff, mxL = -0x7fffffff-1, mnR = 0x7fffffff, mxR = -0x7fffffff-1;
    for (uint32_t i = 0; i < frames; i++) {
        int l = samples[2*i], r = samples[2*i+1];
        if (l < mnL) mnL = l;
        if (l > mxL) mxL = l;
        if (r < mnR) mnR = r;
        if (r > mxR) mxR = r;
    }
    if (frames == 0) { mnL = mxL = mnR = mxR = 0; }
    int mn = std::min(mnL, mnR), mx = std::max(mxL, mxR);
    int data_bits = ofr_bitlen(mn, mx);
    int sh = (32 - data_bits) & 0x1f;
    uint32_t bmask = (bps >= 32) ? 0xffffffffu : ((1u << bps) - 1u);

    const int OD_IDX = p.stereo_order_idx, IV_IDX = p.stereo_interval_idx;
    int max_order = DAT_00326220[OD_IDX];
    int right_order = DAT_00326200[OD_IDX];
    int interval = IVT[IV_IDX];

    enc.encode_split((uint32_t)bps, (uint32_t)mnL & bmask); enc.encode_split((uint32_t)bps, (uint32_t)mxL & bmask); enc.encode_bits(1, 0);
    enc.encode_split((uint32_t)bps, (uint32_t)mnR & bmask); enc.encode_split((uint32_t)bps, (uint32_t)mxR & bmask); enc.encode_bits(1, 0);
    std::vector<int32_t> res(2 * (size_t)frames);

    if (p.pred == 2 || p.pred == 3) {
        const bool cascade_only = p.pred == 2;
        CascadeConfig cfg;
        cfg.stage_idx = { 2, 3 };              // size1/size2 from CASC_OT/CASC_S2
        std::vector<int> st_s1 = { CASC_OT[cfg.stage_idx[0]], CASC_OT[cfg.stage_idx[1]] };
        std::vector<int> st_s2 = { CASC_S2[cfg.stage_idx[0]], CASC_S2[cfg.stage_idx[1]] };

        if (!cascade_only) {
            enc.encode_bits(12, CascadeConfig::MAIN_W - 2);
            enc.encode_bits(3, (uint32_t)IV_IDX);
            enc.encode_bits(5, (uint32_t)OD_IDX);
        }
        cfg.write(enc);

        std::vector<uint8_t> schL(2, 1), schR(2, 1);
        if (!cascade_only) {
            // two all-cascade schedules
            int firstcd = std::min((int)max_order - (int)right_order + 1, (int)frames);
            int seg_len = interval;
            int nseg = ((int)frames + (~firstcd) + seg_len) / seg_len;
            if (nseg < 0) nseg = 0;
            schL.assign((size_t)nseg + 2, 1);
            schR.assign((size_t)nseg + 2, 1);
            OFR_ModelContext cl0, cl1, cr0, cr1;
            cl0.init(2, 0x8000); cl1.init(2, 0x8000); cr0.init(2, 0x8000); cr1.init(2, 0x8000);
            OFR_ModelContext* cl[2] = { &cl0, &cl1 }; OFR_ModelContext* cr[2] = { &cr0, &cr1 };
            enc.encode_symbol(*cl[0], schL[0]); uint32_t pL = schL[0];
            enc.encode_symbol(*cr[0], schR[0]); uint32_t pR = schR[0];
            for (int i = 1; i <= nseg; ++i) {
                enc.encode_symbol(*cl[pL], schL[i]); pL = schL[i];
                enc.encode_symbol(*cr[pR], schR[i]); pR = schR[i];
            }
        }

        // the predictor state is far too large for a worker thread's stack
        auto cs = std::make_unique<OFR_PredictorCascadeStereo>();
        cs->setup_for_encode(mnL, mxL, mnR, mxR, data_bits, (int)(2 * frames),
                             CascadeConfig::MAIN_W, (uint32_t)interval, (uint32_t)max_order, (uint32_t)right_order,
                             CascadeConfig::N_STAGES, 1.0, CascadeConfig::FC_W, CascadeConfig::K, CascadeConfig::GOLOMB,
                             st_s1, st_s2, cfg.stage_mu10, schL, schR);
        cs->cascade_only = cascade_only;
        cs->cascade_init(); cs->need_init = false;
        for (uint32_t f = 0; f < frames; f++) {
            int L = samples[2*f], R = samples[2*f+1];
            if (cascade_only) {
                res[2*f] = residual_of(L, cs->sub_predict(cs->casL, cs->ringsA, cs->ringsB), mnL, mxL, sh);
                cs->sub_update(cs->casL, cs->ringsA, cs->ringsB, (double)L);
                res[2*f+1] = residual_of(R, cs->sub_predict(cs->casR, cs->ringsB, cs->ringsA), mnR, mxR, sh);
                cs->sub_update(cs->casR, cs->ringsB, cs->ringsA, (double)R);
                continue;
            }
            if (cs->mode_L == 0) {
                res[2*f] = residual_of(L, round_to_int32_cvtsd2si(cs->main.predictLeft()), mnL, mxL, sh);
                cs->main.updateLeft((double)L);
                cs->sub_predict(cs->casL, cs->ringsA, cs->ringsB);
                cs->sub_update(cs->casL, cs->ringsA, cs->ringsB, (double)L);
            } else {
                res[2*f] = residual_of(L, cs->sub_predict(cs->casL, cs->ringsA, cs->ringsB), mnL, mxL, sh);
                cs->sub_update(cs->casL, cs->ringsA, cs->ringsB, (double)L);
                cs->main.updateLeft((double)L);
            }
            if (cs->mode_R == 0) {
                res[2*f+1] = residual_of(R, round_to_int32_cvtsd2si(cs->main.predictRight()), mnR, mxR, sh);
                cs->main.updateRight((double)R);
                cs->sub_predict(cs->casR, cs->ringsB, cs->ringsA);
                cs->sub_update(cs->casR, cs->ringsB, cs->ringsA, (double)R);
            } else {
                res[2*f+1] = residual_of(R, cs->sub_predict(cs->casR, cs->ringsB, cs->ringsA), mnR, mxR, sh);
                cs->sub_update(cs->casR, cs->ringsB, cs->ringsA, (double)R);
                cs->main.updateRight((double)R);
            }
            if (--cs->cc_count == 0) { cs->mode_L = cs->schedL[cs->sched_idx]; cs->mode_R = cs->schedR[cs->sched_idx]; cs->cc_count = cs->seg_len; cs->sched_idx++; }
        }
    } else {
        enc.encode_bits(12, p.pred_weight - 2);
        enc.encode_bits(3, (uint32_t)IV_IDX);
        enc.encode_bits(5, (uint32_t)OD_IDX);

        auto inner = std::make_unique<OFR_PredictorStereo_Inner>();
        inner->init((double)(p.pred_weight - 1) / (double)p.pred_weight, max_order, max_order - right_order, (uint32_t)interval);
        for (uint32_t f = 0; f < frames; f++) {
            int L = samples[2*f], R = samples[2*f+1];
            res[2*f] = residual_of(L, round_to_int32_cvtsd2si(inner->predictLeft()), mnL, mxL, sh);
            inner->updateLeft((double)L);
            res[2*f+1] = residual_of(R, round_to_int32_cvtsd2si(inner->predictRight()), mnR, mxR, sh);
            inner->updateRight((double)R);
        }
    }

    encode_entropy(enc, p, 2, data_bits, res);
    enc.flush();
}

static std::vector<uint8_t> encode_one_config(const int32_t* samples, uint32_t frames, int channels, int bps,
                                              uint8_t sample_type, const OFR_EncodeParams& p) {
    OFR_RangeEncoder enc;
    if (channels == 2) encode_stereo_payload(enc, samples, frames, bps, p);
    else encode_mono_payload(enc, samples, frames, bps, p);

    uint32_t values = frames * (uint32_t)channels;
    uint16_t reserved = (uint16_t)(((uint16_t)p.ent << 11) | ((uint16_t)p.pred << 6) | 1u);   // ent, pred, post=1

    std::vector<uint8_t> payload;                       // D-4 bytes (everything after the CRC field)
    payload.reserve(enc.out.size() + 16);
    ofr_put32(payload, values);
    payload.push_back(sample_type);
    payload.push_back((uint8_t)(channels - 1));
    payload.push_back((uint8_t)(reserved & 0xff)); payload.push_back((uint8_t)(reserved >> 8));
    payload.push_back(0x85);                            // encoder ID byte 0 (decoder skips it)
    payload.insert(payload.end(), enc.out.begin(), enc.out.end());   // range stream (out[0] = dummy b0)

    std::vector<uint8_t> comp;
    comp.reserve(payload.size() + 12);
    put_tag(comp, OFR_TAG_COMP);
    ofr_put32(comp, 4 + (uint32_t)payload.size());
    ofr_put32(comp, ofr_crc32(payload.data(), payload.size()));
    comp.insert(comp.end(), payload.begin(), payload.end());
    return comp;
}

// the configurations OFR_EncodeParams::search tries on each block, all of them verified against
// the reference decoder
static std::vector<OFR_EncodeParams> search_candidates(const OFR_EncodeParams& base, int channels) {
    std::vector<OFR_EncodeParams> out;
    for (int ent = 1; ent <= 3; ++ent) {
        OFR_EncodeParams c = base;
        c.ent = ent;
        c.pred = 1;
        if (channels == 2) {
            for (int od : {0, 1}) { c.stereo_order_idx = od; out.push_back(c); }
        } else {
            for (int order : {24, 64, 96}) { c.mono_order = order; out.push_back(c); }
        }
        c.pred = 3;
        out.push_back(c);
    }
    return out;
}

OFR_EncodedBlock ofr_encode_comp_block(const int32_t* samples, uint32_t frames, int channels, int bps,
                                       uint8_t sample_type, const OFR_EncodeParams& params) {
    OFR_EncodedBlock block;
    if (!params.search) {
        block.comp = encode_one_config(samples, frames, channels, bps, sample_type, params);
        return block;
    }
    for (const OFR_EncodeParams& c : search_candidates(params, channels)) {
        std::vector<uint8_t> comp = encode_one_config(samples, frames, channels, bps, sample_type, c);
        if (block.comp.empty() || comp.size() < block.comp.size()) block.comp = std::move(comp);
    }
    return block;
}

void ofr_assemble_file(bool is_float, uint64_t total_values, uint8_t sample_type, int channels, uint32_t samplerate,
                       const std::vector<OFR_EncodedBlock>& blocks, std::vector<uint8_t>& file,
                       const std::vector<uint8_t>& head, const std::vector<uint8_t>& tail) {
    size_t size = 64 + head.size() + tail.size();
    for (const OFR_EncodedBlock& b : blocks) size += b.comp.size() + b.deta.size();

    file.clear();
    file.reserve(size);
    put_tag(file, is_float ? OFR_TAG_OFRX : OFR_TAG_OFR);
    ofr_put32(file, 17);                                 // main block size
    ofr_put32(file, (uint32_t)total_values);
    put16(file, (uint16_t)(total_values >> 32));
    file.push_back(sample_type);
    file.push_back((uint8_t)(channels - 1));             // channel config
    ofr_put32(file, samplerate);
    put16(file, 0x2585);                                 // encoder ID (informational)
    file.push_back(0x02);                                // compression
    put16(file, 20);                                     // minDecoderVersion - 4500

    put_tag(file, OFR_TAG_HEAD);
    ofr_put32(file, (uint32_t)head.size());
    file.insert(file.end(), head.begin(), head.end());

    for (const OFR_EncodedBlock& b : blocks) {
        file.insert(file.end(), b.comp.begin(), b.comp.end());
        file.insert(file.end(), b.deta.begin(), b.deta.end());
    }

    put_tag(file, OFR_TAG_TAIL);
    ofr_put32(file, (uint32_t)tail.size());
    file.insert(file.end(), tail.begin(), tail.end());
}

// a block's value count has to fit the decoder's signed 32-bit arithmetic
uint32_t ofr_effective_block_frames(const OFR_EncodeParams& params, uint64_t frames, int channels) {
    const uint64_t limit = (1u << 30) / (uint32_t)channels;
    uint64_t block = params.block_frames ? params.block_frames : std::max<uint64_t>(frames, 1);
    return (uint32_t)std::min(block, limit);
}

void ofr_parallel_for(size_t count, uint32_t threads, const std::function<void(size_t)>& job) {
    std::atomic<size_t> next{0};
    std::exception_ptr failure;
    std::mutex failure_lock;

    auto worker = [&] {
        OFR_ScopedFpEnv fp_env;
        for (size_t i = next++; i < count; i = next++) {
            try {
                job(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(failure_lock);
                if (!failure) failure = std::current_exception();
                next = count;
            }
        }
    };

    size_t extra = std::min<size_t>(count, std::max(threads, 1u)) - (count ? 1 : 0);
    std::vector<std::thread> pool;
    for (size_t t = 0; t < extra; t++) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    if (failure) std::rethrow_exception(failure);
}

static bool valid_params(const OFR_EncodeParams& p, int channels, int bps) {
    if (channels != 1 && channels != 2) return false;
    if (bps != 8 && bps != 16 && bps != 24 && bps != 32) return false;
    if (p.pred < 1 || p.pred > 3 || p.ent < 1 || p.ent > 3) return false;
    if (p.pred_weight < 2 || p.pred_weight > 4096 || p.ent_weight < 2 || p.ent_weight > 4096) return false;
    if (channels == 1) return p.mono_order >= 1 && p.mono_order <= OFR_MAX_ORDER && p.mono_interval >= 1 && p.mono_interval <= 0x10000;
    if (p.stereo_interval_idx < 0 || p.stereo_interval_idx > 6 || p.stereo_order_idx < 0 || p.stereo_order_idx > 30) return false;
    int max_order = DAT_00326220[p.stereo_order_idx], right_order = DAT_00326200[p.stereo_order_idx];
    return max_order >= 1 && max_order <= OFR_MAX_ORDER && right_order <= max_order - right_order;
}

bool ofr_encode(const int32_t* samples, uint64_t frames, int channels, uint32_t samplerate, int bps,
                const OFR_EncodeParams& params, std::vector<uint8_t>& file,
                const std::vector<uint8_t>& head, const std::vector<uint8_t>& tail) {
    if (!valid_params(params, channels, bps)) return false;

    static constexpr uint8_t signed_type[] = {1, 3, 5, 7};   // SINT8..SINT32
    const uint8_t sample_type = (uint8_t)(signed_type[bps / 8 - 1] - (params.unsigned_samples ? 1 : 0));

    const uint32_t per_block = ofr_effective_block_frames(params, frames, channels);
    const size_t block_count = frames ? (size_t)((frames + per_block - 1) / per_block) : 1;
    std::vector<OFR_EncodedBlock> blocks(block_count);

    ofr_parallel_for(block_count, params.threads, [&](size_t i) {
        uint64_t first = (uint64_t)i * per_block;
        uint32_t n = (uint32_t)std::min<uint64_t>(per_block, frames - first);
        blocks[i] = ofr_encode_comp_block(samples + first * (uint64_t)channels, n, channels, bps, sample_type, params);
    });

    ofr_assemble_file(false, frames * (uint64_t)channels, sample_type, channels, samplerate, blocks, file, head, tail);
    return true;
}
