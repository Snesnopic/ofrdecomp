// pred_type=3 — dual predictor: standard LPC + cascade NLMS (presets 4..max).
// Compiled WITHOUT -ffast-math (see CMakeLists) for float32 bit-exactness.
// Full RE map in doc/pred3_analysis.md.
//
// STATUS: mono init (params + entropy-coded schedule) implemented and verifiable.
// Cascade stage FIR + per-stage NLMS implemented. Final combiner (FUN_00015890/
// FUN_000154e0) is best-effort pending RE of where the 0x268 coefs are solved.

#include "../include/optimfrog_decoder.h"
#include "optimfrog_tables.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// --- adaptive 2-symbol Markov context for the segment schedule (FUN_0000fb90 family) ---
namespace {
struct SchedCtx {
    std::vector<uint32_t> freqs;
    uint32_t num_symbols, total_freq, limit, num_nodes;
};
static uint32_t sched_rebuild(std::vector<uint32_t>& f, uint32_t node, uint32_t nn) {
    if (node < nn) {
        uint32_t l = sched_rebuild(f, node * 2, nn);
        uint32_t r = sched_rebuild(f, node * 2 + 1, nn);
        f[node] = l;
        return l + r;
    }
    return f[node];
}
static void sched_init(SchedCtx& c, uint32_t num_symbols, uint32_t limit) {
    c.num_symbols = num_symbols;
    uint32_t nn = 1; while (nn < num_symbols) nn <<= 1;
    c.num_nodes = nn; c.limit = limit;
    c.freqs.assign(nn * 2, 0u);
    for (uint32_t i = 0; i < num_symbols; ++i) c.freqs[nn + i] = 1u;
    c.total_freq = num_symbols;
    sched_rebuild(c.freqs, 1, nn);
}
static void sched_halve(SchedCtx& c) {
    c.total_freq = 0;
    for (uint32_t i = 0; i < c.num_symbols; ++i) {
        uint32_t& f = c.freqs[c.num_nodes + i];
        f = ((f - 1u) >> 1) + 1u;
        c.total_freq += f;
    }
    sched_rebuild(c.freqs, 1, c.num_nodes);
}
static uint32_t sched_decode(SchedCtx& c, OFR_RangeCoder* rc) {
    rc->normalize();
    uint32_t step = rc->range / c.total_freq;
    uint32_t v = rc->value / step;
    uint32_t bound = (v < c.total_freq) ? v : c.total_freq - 1u;
    auto& f = c.freqs;
    uint32_t node = 1, acc = 0;
    do {
        uint32_t nv = f[node];
        uint32_t s = nv + acc;
        uint32_t child;
        if (bound < s) { f[node] = nv + 2u; child = node; }
        else { child = node + 1u; acc = s; }
        node = node + child;
    } while (node < c.num_nodes);
    uint32_t leaf = f[node];
    uint32_t accstep = step * acc;
    rc->value -= accstep;
    if (acc + leaf < c.total_freq) rc->range = step * leaf;
    else rc->range -= accstep;
    f[node] += 2u;
    c.total_freq += 2u;
    if (c.limit <= c.total_freq) sched_halve(c);
    return node - c.num_nodes;
}
} // namespace

static const double DAT_196e0 = -1.0;
static const double DAT_19688 = 1000.0;
static const double DAT_19718 = 1.0 / 1024.0;
// cascade stage order table (DAT_21ea2)
static const int cascade_order_tbl[] = {512,256,128,64,128,64,32,16,0,0,0,0,0,0,0,0,
                                        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

void OFR_PredictorCascadeMono::init(OFR_RangeCoder* rc, uint32_t bit_depth, int mn, int mx, int dbits, int total) {
    min_val = mn; max_val = mx; shift = 32 - dbits; total_samples = total;

    // --- main LPC params (same as pred=1) ---
    uint32_t w = rc->read_uniform_bits(12);
    main_weight_param = (w == 0xfff) ? (rc->read_uniform_bits(16) + 0x1001) : (w + 2);

    uint32_t iv = rc->read_uniform_bits(3);
    main_interval = (iv == 7) ? (rc->read_uniform_bits(16) + 1) : (uint32_t)DAT_00326238[iv];

    uint32_t od = rc->read_uniform_bits(5);
    main_order = (od == 0x1f) ? (rc->read_uniform_bits(8) + 1) : (uint32_t)DAT_00326220[od];

    // --- cascade params ---
    n_stages = (int)rc->read_uniform_bits(3) + 1;             // 0x437a0

    // 0x43798 decay scale: 1 bit; if set, read 10-bit/1024 else 1.0
    double cc_decay_scale;
    {
        rc->normalize();
        uint32_t half = rc->range >> 1;
        if (rc->value < half) { rc->range = half; cc_decay_scale = 1.0; }
        else {
            rc->value -= half; rc->range -= half;
            uint32_t idx = rc->read_uniform_bits(10);
            cc_decay_scale = (double)idx * DAT_19718;
        }
    }
    decay = cc_decay_scale;

    uint32_t fw = rc->read_uniform_bits(12);                  // 0x437a4 final weight
    uint32_t fc_weight_param = (fw == 0xfff) ? (rc->read_uniform_bits(16) + 0x1001) : (fw + 2);

    uint32_t k = rc->read_uniform_bits(3);                    // 0x437a8 = 1<<k
    fc_halve_interval = 1u << k;

    // 0x437ac: 1 bit; if set read16 else 0x40
    {
        rc->normalize();
        uint32_t half = rc->range >> 1;
        if (rc->value < half) { rc->range = half; /* 0x437ac = 0x40, unused field */ }
        else { rc->value -= half; rc->range -= half; rc->read_uniform_bits(16); }
    }

    // per-stage order + mu
    stages.assign(n_stages + 1, OFR_CascadeStage());
    double stage_eps = std::max({(double)std::abs(min_val), (double)std::abs(max_val), 1.0});
    for (int s = 1; s <= n_stages; ++s) {
        uint32_t o5 = rc->read_uniform_bits(5);
        int order = (o5 == 0x1f) ? ((int)rc->read_uniform_bits(8) * 4 + 4) : cascade_order_tbl[o5];
        uint32_t mu10 = rc->read_uniform_bits(10);
        double mu = (double)mu10 / DAT_19688;

        OFR_CascadeStage& st = stages[s];
        st.order = order;
        st.mu = mu;
        st.eps = stage_eps;
        int wpad = std::max(8, order);
        st.weights.assign(wpad, 0.0f);
        st.energy = 0.0;
        st.ring.assign(0x400, 0.0f);
        st.ring_copy = order + 1;
        st.ring_cur = order + 1;
    }

    seg_len = (int)main_interval;   // 0x436ac

    // final combiner
    fc_size = n_stages;
    fc_decay = ((double)fc_weight_param - 1.0) / (double)fc_weight_param;
    fc_coefs.assign(std::max(1, fc_size), 0.0);
    if (fc_size > 0) fc_coefs[0] = 1.0;
    fc_counter = 0; fc_last_halve = 0;
    fc_mat.assign(std::max(1, fc_size), std::vector<double>(std::max(1, fc_size), 0.0));
    fc_v.assign(std::max(1, fc_size) + 1, 0.0);

    // --- decode the entropy-coded per-segment schedule (0x437b8) ---
    int order_p1 = (int)main_order + 1;
    int firstcd = std::min(order_p1, total);
    int nseg = (total + (~firstcd) + seg_len) / seg_len;   // matches binary integer math
    if (nseg < 0) nseg = 0;
    schedule.assign((size_t)nseg + 2, 0);

    SchedCtx sctx[2];
    sched_init(sctx[0], 2, 0x8000);
    sched_init(sctx[1], 2, 0x8000);
    uint32_t prev = sched_decode(sctx[0], rc);
    schedule[0] = (uint8_t)prev;
    for (int i = 1; i <= nseg; ++i) {   // binary decodes N+1 symbols total
        uint32_t sym = sched_decode(sctx[prev], rc);
        schedule[i] = (uint8_t)sym;
        prev = sym;
    }
    schedule[nseg + 1] = 1;  // sentinel

    cc_count = firstcd;          // 0x436b4
    cc_mode = schedule[0];       // 0x436b0
    sched_idx = 1;               // 0x436b8
    need_init = true;

}

void OFR_PredictorCascadeMono::cascade_init() {
    bias = 0.0;
    stage_pred.assign(n_stages + 2, 0.0);
    errv.assign(n_stages + 2, 0.0);
    cumsum.assign(n_stages + 2, 0.0);
    // main LPC
    main_lpc.init((int)main_order, (int)main_interval,
                  ((double)main_weight_param - 1.0) / (double)main_weight_param);
    // stages already allocated in init()
}

// FUN_0000f850: stage FIR (float32, x8 unroll, 4 accumulators)
static double stage_fir(const OFR_CascadeStage& st) {
    const float* w = st.weights.data();
    const float* h = &st.ring[st.ring_cur - st.order];
    // match the SSE accumulation grouping of FUN_0000f850 exactly (float is not associative):
    //   acc[j] = h[j+4]*w[j+4] + (h[j]*w[j] + acc[j])
    float a5 = 0.f, a6 = 0.f, a7 = 0.f, a8 = 0.f;
    int n = st.order;
    for (int i = 0; i < n; i += 8) {
        a5 = h[i+4]*w[i+4] + (h[i+0]*w[i+0] + a5);
        a6 = h[i+5]*w[i+5] + (h[i+1]*w[i+1] + a6);
        a7 = h[i+6]*w[i+6] + (h[i+2]*w[i+2] + a7);
        a8 = h[i+7]*w[i+7] + (h[i+3]*w[i+3] + a8);
    }
    // horizontal sum, pairwise as the asm: ((a8+a7) + (a6+a5)), each promoted to double
    return ((double)a8 + (double)a7) + ((double)a6 + (double)a5);
}

// FUN_0000f8c0: per-stage NLMS weight update
static void stage_nlms_update(OFR_CascadeStage& st, float res) {
    int n = st.order;
    float* h = &st.ring[st.ring_cur - n];
    // sliding energy: e = h[-1]^2 + ((double)energy_int - h[-(n+1)]^2); store TRUNCATED to int
    double hnew = (double)st.ring[st.ring_cur - 1];        // h[-1] relative to cur
    double hold = (double)st.ring[st.ring_cur - 1 - n];    // oldest leaving sample
    double e = hnew * hnew + (st.energy - hold * hold);
    st.energy = e;
    float step = (float)(((double)res * st.mu) / (e + st.eps));
    float* w = st.weights.data();
    for (int i = 0; i < n; ++i) w[i] += h[i] * step;
}

// FUN_000152c0: LDLT solve, row stride 8 doubles, diag threshold 2^-17.
// matrix lower-tri in/out (L factor), rhs in/out (solution), diag scratch.
static bool ldlt8(double* m, double* rhs, double* diag, int size) {
    const uint64_t absmask = 0x7fffffffffffffffull;
    const double thr = 7.62939453125e-06; // 2^-17 (DAT_19768)
    auto absd = [&](double x){ uint64_t u; std::memcpy(&u,&x,8); u&=absmask; double r; std::memcpy(&r,&u,8); return r; };
    double d0 = m[0];
    diag[0] = d0;
    if (absd(d0) < thr) return false;
    m[0] = 1.0;
    if (size > 1) {
        for (int i = 1; i < size; ++i) {
            double* row = m + i*8;
            for (int j = 0; j < i; ++j) {
                double v = row[j];
                for (int k = 0; k < j; ++k) v -= row[k] * diag[k] * m[j*8 + k];
                row[j] = v / diag[j];
            }
            double dv = m[i*8 + i];
            m[i*8 + i] = 1.0;
            for (int k = 0; k < i; ++k) dv -= row[k] * row[k] * diag[k];
            diag[i] = dv;
            if (absd(dv) < thr) return false;
        }
        for (int i = 1; i < size; ++i) {
            double v = rhs[i];
            for (int k = 0; k < i; ++k) v -= m[i*8 + k] * rhs[k];
            rhs[i] = v;
        }
    }
    rhs[size-1] /= diag[size-1];
    for (int i = size-2; i >= 0; --i) {
        double v = rhs[i] / diag[i];
        for (int k = i+1; k < size; ++k) v -= m[k*8 + i] * rhs[k];
        rhs[i] = v;
    }
    return true;
}

void OFR_PredictorCascadeMono::fc_solve() {
    if (fc_v[0] >= 0.5) {   // DAT_19770
        double W[64] = {0}; double rhs[8] = {0}; double diag[8] = {0};
        for (int u = 0; u < fc_size; ++u)
            for (int k = 0; k <= u; ++k) W[u*8 + k] = fc_mat[u][k];
        for (int j = 0; j < fc_size; ++j) rhs[j] = fc_v[j + 1];
        if (ldlt8(W, rhs, diag, fc_size)) {
            for (int i = 0; i < fc_size; ++i) fc_coefs[i] = rhs[i];
        } else {
            for (int i = 0; i < fc_size; ++i) fc_coefs[i] = 0.0;
            // fallback: pick the coef whose V equals V[0], else coef0
            int idx = 0;
            for (int j = 0; j < fc_size; ++j)
                if (fc_v[0] == fc_v[j + 1]) { idx = j; break; }
            fc_coefs[idx] = 1.0;
        }
    } else {
        if (fc_v[0] != 0.0 && fc_v[0] < 9.31322574615478516e-10 /*2^-30*/) {
            for (int u = 0; u < fc_size; ++u)
                for (int k = 0; k <= u; ++k) fc_mat[u][k] = 0.0;
            for (int j = 0; j <= fc_size; ++j) fc_v[j] = 0.0;
        }
        for (int i = 0; i < fc_size; ++i) fc_coefs[i] = 0.0;
        fc_coefs[0] = 1.0;
    }
}

int OFR_PredictorCascadeMono::cascade_predict() {
    cumsum[0] = 0.0;
    for (int s = 1; s <= n_stages; ++s) {
        OFR_CascadeStage& st = stages[s];
        st.ring_cur++;
        if (st.ring_cur == 0x400) {
            for (int kk = 0; kk < st.ring_copy; ++kk)
                st.ring[kk] = st.ring[0x400 - st.ring_copy + kk];
            st.ring_cur = st.ring_copy;
        }
        st.ring[st.ring_cur] = 0.0f;
        double sp = stage_fir(st);
        stage_pred[s] = sp;
        cumsum[s] = sp + cumsum[s - 1];
    }
    // final combiner predict: dot(cumsum[n..n-fc_size+1], fc_coefs) + bias
    double fin = 0.0;
    for (int i = 0; i < fc_size; ++i) fin += cumsum[n_stages - i] * fc_coefs[i];
    fin += bias;
    return (int)std::lrint(fin);
}

void OFR_PredictorCascadeMono::cascade_update(double actual) {
    double err = actual - bias;
    bias = actual * decay;
    errv[0] = err;
    double e = err;
    for (int s = 1; s <= n_stages; ++s) {
        e -= stage_pred[s];
        errv[s] = e;
        stage_nlms_update(stages[s], (float)e);
        stages[s].ring[stages[s].ring_cur] = (float)errv[s - 1];
    }
    // final combiner update (FUN_00015890): exp-weighted covariance M + cross-corr V over the
    // cascade cumulative outputs; periodic LDLT re-solve for fc_coefs.
    fc_counter++;
    for (int u = 0; u < fc_size; ++u) {
        double hu = cumsum[n_stages - u];
        for (int k = 0; k <= u; ++k)
            fc_mat[u][k] = cumsum[n_stages - k] * hu + fc_mat[u][k] * fc_decay;
    }
    fc_v[0] = err * err + fc_v[0] * fc_decay;
    for (int j = 1; j <= fc_size; ++j)
        fc_v[j] = err * cumsum[n_stages - (j - 1)] + fc_v[j] * fc_decay;
    if (fc_halve_interval + fc_last_halve <= fc_counter) {
        fc_solve();
        fc_last_halve = fc_counter;
    }
}

void OFR_PredictorCascadeMono::decode(int32_t* dest, uint32_t count) {
    if (need_init) {
        cascade_init();
        sample_counter = 0xac44;
        need_init = false;
    }
    int sh = shift & 0x1f;
    for (uint32_t i = 0; i < count; ++i) {
        if (--sample_counter == 0) sample_counter = 0xac44;
        int res = dest[i];
        int out;
        if (cc_mode == 0) {
            int p = (int)std::lrint(main_lpc.predict());
            int cp = std::max(min_val, std::min(p, max_val));
            out = ((cp + res) << sh) >> sh;
            dest[i] = out;
            main_lpc.update((double)out);
            cascade_predict();
            cascade_update((double)out);
        } else {
            int p = cascade_predict();
            int cp = std::max(min_val, std::min(p, max_val));
            out = ((cp + res) << sh) >> sh;
            dest[i] = out;
            cascade_update((double)out);
            main_lpc.update((double)out);
        }
        if (--cc_count == 0) {
            cc_mode = schedule[sched_idx];
            cc_count = seg_len;
            sched_idx++;
        }
    }
}
