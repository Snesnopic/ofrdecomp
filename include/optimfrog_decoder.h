#ifndef OPTIMFROG_DECODER_H
#define OPTIMFROG_DECODER_H

#include "OptimFROG.h"
#include "ofr_common.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <array>
#include <vector>

// buffered reader over a ReadInterface, keeping track of the absolute stream position.
class OFR_BitStream {
public:
    OFR_BitStream(ReadInterface* rInt, void* readerInstance);

    // returns fewer than count bytes only at end of stream
    size_t read(void* dst, size_t count);
    void read_exact(void* dst, size_t count);   // throws OFR_StreamError at end of stream
    // appends up to count bytes, growing as data arrives so that a corrupt length field can't
    // force a huge allocation
    size_t read_append(std::vector<uint8_t>& dst, uint64_t count);

    uint8_t read_u8() { uint8_t v; read_exact(&v, 1); return v; }
    // like read_u32, but reads zeros at end of stream instead of throwing
    uint32_t peek_tag();
    uint16_t read_u16();
    uint32_t read_u32();

    uint64_t tell() const { return pos_; }
    bool seek(uint64_t byte_pos);
    bool skip(uint64_t count);
    bool seekable() const;
    int64_t length() const;
    void close();

private:
    ReadInterface* rInt_;
    void* inst_;
    std::vector<uint8_t> buffer_;
    size_t buf_pos_ = 0;
    size_t buf_avail_ = 0;
    uint64_t pos_ = 0;
    bool open_ = true;
};


struct OFR_ModelContext {
    std::vector<uint32_t> freqs;
    uint32_t num_symbols = 0;
    uint32_t total_freq = 0;
    uint32_t limit = 0;
    uint32_t num_nodes = 0;

    void init(uint32_t symbols, uint32_t lim);
    void halve_freqs();
    uint32_t rebuild_tree(uint32_t node);
};

// decodes an in-memory payload. a damaged block can read on past its end: the reference decodes
// straight from the file, so it then sees the bytes that follow the block (the next block is found
// again from its stored offset), and zeros once the file is exhausted. spill provides those bytes.
// past the end of the file, reading stops at a limit the caller derives from the block length:
// high enough not to change the output, low enough that a corrupt state can't spin forever.
struct OFR_RangeCoder {
    static constexpr uint64_t MAX_OVERRUN = 64;

    uint32_t value = 0;
    uint32_t range = 0;
    uint8_t  cache = 0;
    const uint8_t* cur = nullptr;
    const uint8_t* end = nullptr;
    OFR_BitStream* spill = nullptr;
    uint64_t overrun = 0;
    uint64_t overrun_limit = MAX_OVERRUN;

    void init(const uint8_t* data, size_t size, OFR_BitStream* spill_from = nullptr, uint64_t limit = MAX_OVERRUN);

    uint8_t next_byte() {
        if (cur != end) return *cur++;
        return next_byte_past_end();
    }
    uint8_t next_byte_past_end();

    void normalize() {
        while (range < 0x800001) {
            value = ((cache & 1u) << 7) | (value << 8);
            cache = next_byte();
            value |= (uint32_t)(cache >> 1);
            range <<= 8;
        }
    }

    uint32_t read_uniform_split(uint32_t bits) {
        if (bits <= 12) return read_uniform_bits(bits);
        uint32_t val1 = read_uniform_bits(12);
        uint32_t val2 = read_uniform_split(bits - 12);
        return (val2 << 12) | val1;
    }

    uint32_t read_uniform_bits(uint32_t bits) {
        uint32_t ret = 0;
        uint32_t shift = 0;
        while (bits > 12) {
            ret |= (read_uniform_bits_internal(12) << shift);
            shift += 12;
            bits -= 12;
        }
        if (bits > 0) {
            ret |= (read_uniform_bits_internal(bits) << shift);
        }
        return ret;
    }

    uint32_t read_golomb() {
        uint32_t k = 0;
        while (read_uniform_bits(1) == 0) {
            if (++k >= 32) throw OFR_StreamError("golomb prefix too long");
        }
        if (k == 0) return 0;
        return (1u << k) | read_uniform_bits(k);
    }

    uint32_t read_uniform_bits_internal(uint32_t bits) {
        if (bits == 0) return 0;
        normalize();
        uint32_t step = this->range >> bits;
        uint32_t val = this->value / step;
        uint32_t max_val = (1u << bits) - 1;

        if (val < max_val) {
            this->value -= val * step;
            this->range = step;
        } else {
            val = max_val;
            this->value -= val * step;
            this->range -= val * step;
        }
        return val;
    }

    uint32_t decode_symbol(OFR_ModelContext& ctx) {
        normalize();
        uint32_t step = this->range / ctx.total_freq;
        uint32_t val = this->value / step;
        if (val >= ctx.total_freq) {
            val = ctx.total_freq - 1;
        }

        uint32_t node = 1;
        uint32_t accumulated = 0;

        while (node < ctx.num_nodes) {
            uint32_t left_freq = ctx.freqs[node];
            if (val >= accumulated + left_freq) {
                accumulated += left_freq;
                node = node * 2 + 1;
            } else {
                ctx.freqs[node] += 2;
                node = node * 2;
            }
        }

        uint32_t sym = node - ctx.num_nodes;
        uint32_t freq = ctx.freqs[node];

        this->value -= accumulated * step;
        if (accumulated + freq < ctx.total_freq) {
            this->range = step * freq;
        } else {
            this->range -= accumulated * step;
        }

        ctx.freqs[node] += 2;
        ctx.total_freq += 2;

        if (ctx.total_freq >= ctx.limit) {
            ctx.halve_freqs();
        }

        return sym;
    }

    uint32_t read_12bit_value() {
        uint32_t v = decode_uniform(4096);
        return (v == 0xfff) ? decode_uniform(65536) + 0x1001 : v + 2;
    }

    uint32_t decode_uniform(uint32_t max_val) {
        if (max_val == 0) return 0;
        normalize();
        uint32_t step = this->range / max_val;
        uint32_t val = this->value / step;

        if (val < max_val - 1) {
            this->value -= val * step;
            this->range = step;
        } else {
            val = max_val - 1;
            this->value -= val * step;
            this->range -= val * step;
        }
        return val;
    }
};


class OFR_EntropyDecoder {
public:
    void init(uint32_t depth, uint32_t ch, uint32_t entropy_type);
    void init_from_bitstream(OFR_RangeCoder* rc);
    void decode_block(int32_t* dest, uint32_t count, OFR_RangeCoder* rc);

private:
    // a tree context with its own node count, see FUN_00002bd0
    struct FastCtx {
        std::vector<uint32_t> freqs;
        uint32_t num_symbols = 0;
        uint32_t total_freq = 0;
        uint32_t limit = 0;
        uint32_t num_nodes = 0;
    };

    int32_t decode_one_sample(double& var, OFR_RangeCoder* rc);   // ent=2, FUN_00109ab0
    int32_t fast_decode_sample(double& var, OFR_RangeCoder* rc);  // ent=1, FUN_00005ef0
    static void fast_ctx_init(FastCtx& c, uint32_t num_symbols, uint32_t limit);
    static void fast_ctx_halve(FastCtx& c);

    double weight = 0.0;
    double weight2 = 0.0;
    uint32_t bit_depth = 0;
    uint32_t channels = 1;
    uint32_t type = 0;
    bool needs_init = true;

    double variance = 0.0;
    double variance2 = 0.0;
    std::vector<uint32_t> master_freqs;
    uint32_t master_num_symbols = 32;
    uint32_t master_total_freq = 32;
    uint32_t master_limit = 0x8000;
    uint32_t master_num_nodes = 32;

    std::vector<FastCtx> fast_contexts;
    double fast_var_L = 1.0;
    double fast_var_R = 1.0;
};


class OFR_Predictor {
public:
    int order = 0;
    int update_interval = 0;
    double damping = 0.0;
    uint32_t sample_count = 0;
    uint32_t last_update_count = 0;
    uint32_t fail_count = 0;
    int shift = 0;
    int min_val = -0x800000;
    int max_val = 0x7fffff;

    std::vector<double> history_buffer;
    double* history_head = nullptr;

    std::vector<double> vector_R;
    std::vector<double> weights;
    std::vector<double> temp_D;
    std::vector<std::vector<double>> inst_matrix;
    std::vector<std::vector<double>> cov_matrix;

    void init(OFR_RangeCoder* rc);
    void init(int ord, int interval, double damp);
    double predict();
    void update(double sample);
    void update_weights();
    void update_cov_1();
    bool solve_ldlt();

    void decode(int32_t* dest, uint32_t count);
};


// cross-channel LDLT predictor, with room for odd orders: the unrolled loops read and write a
// few entries past the nominal end
struct OFR_PredictorStereo_Inner {
    static constexpr int HISTORY = 1024;

    uint32_t m_count_left;
    uint32_t m_count_right;
    uint32_t m_update_count_left;
    uint32_t m_update_count_right;
    int32_t m_max_order;
    int32_t m_left_order;
    int32_t m_right_order;
    uint32_t m_update_interval;
    double* m_left_hist_ptr;
    double* m_right_hist_ptr;
    double m_weight;

    double m_R_left[2 * OFR_MAX_ORDER + 8];
    double m_R_right[2 * OFR_MAX_ORDER + 8];

    double m_left_coefs[OFR_MAX_ORDER + 8];
    double m_right_coefs[OFR_MAX_ORDER + 8];
    double m_temp_vector[OFR_MAX_ORDER + 8];

    double m_left_matrix[OFR_MAX_ORDER][OFR_MAX_ORDER];
    double m_right_matrix[OFR_MAX_ORDER][OFR_MAX_ORDER];
    double m_temp_matrix[OFR_MAX_ORDER + 1][OFR_MAX_ORDER];

    double m_left_history[HISTORY + 8];
    double m_right_history[HISTORY + 8];

    void init(double weight, int max_order, int left_order, uint32_t update_interval);
    double predictLeft();
    double predictRight();
    void updateLeft(double sample);
    void updateRight(double sample);

    void updateAutocorrLeft();
    void updateAutocorrRight();
    void solveCholeskyLeft();
    void solveCholeskyRight();
};

// weight, update interval and order of the stereo LPC layer, as coded in the block header
struct OFR_StereoLpcParams {
    uint32_t weight_param = 0;
    uint32_t update_interval = 0;
    int32_t max_order = 0;
    int32_t right_order = 0;
    void read(OFR_RangeCoder* rc);   // throws on an order the predictor can't hold
};

struct OFR_PredictorStereo {
    OFR_PredictorStereo_Inner m_inner;
    OFR_StereoLpcParams m_params;
    int32_t m_left_min = 0, m_left_max = 0, m_right_min = 0, m_right_max = 0;
    uint32_t m_shift = 0;
    bool m_need_init = false;

    void init(OFR_RangeCoder* rc);
    void decode(int32_t* outputs, uint32_t num_samples);
};


// pred_type=3: dual predictor, LPC plus cascade NLMS, see doc/pred3_analysis.md.
// pred_type=2: the same cascade on its own, with neither the LPC layer nor the schedule.
struct OFR_CascadeStage {
    std::vector<float> weights;   // order taps
    int order = 0;
    double energy = 0.0;          // sliding energy (binary stores double via MOVSD)
    double mu = 0.0;
    double eps = 0.0;
    std::vector<float> ring;
    int ring_cur = 0;             // index of current head
    int ring_copy = 0;            // order+1
};

class OFR_PredictorCascadeMono {
public:
    OFR_Predictor main_lpc;       // the +0x10 LPC

    int n_stages = 0;
    std::vector<OFR_CascadeStage> stages;   // 1-based (index 0 unused)
    double bias = 0.0, decay = 1.0;
    std::vector<double> stage_pred;
    std::vector<double> errv;
    std::vector<double> cumsum;

    // final combiner (NLMS over cumulative stage outputs)
    uint32_t fc_counter = 0;
    uint32_t fc_last_halve = 0;
    int fc_size = 0;
    uint32_t fc_halve_interval = 0;
    double fc_decay = 1.0;
    std::vector<double> fc_coefs;
    std::vector<std::vector<double>> fc_mat; // covariance M (lower-tri), exp-weighted
    std::vector<double> fc_v;                // cross-correlation V[0..fc_size]
    void fc_solve();

    int min_val = 0, max_val = 0, shift = 0;
    int total_samples = 0;
    uint32_t main_weight_param = 0, main_order = 0, main_interval = 0;

    bool cascade_only = false;  // pred_type=2
    int seg_len = 0;
    int cc_count = 0;           // countdown to the next schedule entry
    int cc_mode = 0;
    uint32_t sched_idx = 0;
    std::vector<uint8_t> schedule;
    bool need_init = false;

    void init(OFR_RangeCoder* rc, int mn, int mx, int dbits, int total);
    void init_cascade_only(OFR_RangeCoder* rc, int mn, int mx, int dbits);
    void decode(int32_t* dest, uint32_t count);

    // encoder-side setup: mirror init() from explicit params
    void setup_for_encode(int mn, int mx, int dbits, int total,
                          uint32_t main_w, uint32_t main_iv, uint32_t main_od, int n_stages_,
                          double decay_, uint32_t fc_w, uint32_t fc_halve_k, uint32_t golomb_field,
                          const std::vector<int>& stage_orders, const std::vector<int>& stage_mu10,
                          const std::vector<uint8_t>& sched);

    void cascade_init();
    int  cascade_predict();
    void cascade_update(double actual);

private:
    void read_cascade_params(OFR_RangeCoder* rc);
};

// pred_type=4: DualStream (.ofs) adaptive quantized-domain DPCM, see doc/dualstream_analysis.md.
// `--correction` files are not supported: their range coder object is null for ordinary .ofs
// files, which short-circuits that whole branch of the reference.
struct OFR_PredictorDualStreamMono {
    OFR_Predictor main_lpc;
    int shift = 0;

    uint32_t weight_param = 0;            // step-schedule advance interval
    std::vector<int32_t> step_schedule;
    uint32_t step_idx = 0;
    int32_t current_step = 1;
    uint32_t sample_counter = 0;          // persists across decode() calls within a block

    void init(OFR_RangeCoder* rc, int dbits, uint32_t total);
    void decode(int32_t* dest, uint32_t count);
};

// same, doubled, on OFR_PredictorStereo_Inner: two step schedules, one per channel, decoded
// interleaved from one shared context (FUN_1000201b0 / FUN_1000206b0 in the ofs binary).
struct OFR_PredictorDualStreamStereo {
    OFR_PredictorStereo_Inner main;
    int shift = 0;

    uint32_t weight_param = 0;
    std::vector<int32_t> step_schedule_L, step_schedule_R;
    uint32_t step_idx = 0;
    int32_t current_step_L = 1, current_step_R = 1;
    uint32_t sample_counter = 0;

    void init(OFR_RangeCoder* rc, int dbits, uint32_t total);
    void decode(int32_t* dest, uint32_t count);
};

// pred_type=2/3 stereo: two cross-channel cascades, plus OFR_PredictorStereo_Inner for pred_type=3.
struct OFR_CascadeStageX {        // cross-channel NLMS stage
    std::vector<float> w1, w2;    // this-channel taps, other-channel taps
    int size1 = 0, size2 = 0;
    double energy = 0.0, mu = 0.0, eps = 0.0;
};
struct OFR_CascadeRing {
    std::vector<float> ring;
    int cur = 0, copy = 0;        // head index, copy count (=max(size1+1,size2))
};
struct OFR_SubCascade {           // one channel's cascade (predicts via primary+secondary ring)
    int n_stages = 0;
    std::vector<OFR_CascadeStageX> stages;  // 1-based
    double bias = 0.0, decay = 1.0;
    std::vector<double> stage_pred, cumsum, errv;
    uint32_t fc_counter = 0, fc_last_halve = 0, fc_halve_interval = 0;
    int fc_size = 0; double fc_decay = 1.0;
    std::vector<double> fc_coefs, fc_v;
    std::vector<std::vector<double>> fc_mat;
};

class OFR_PredictorCascadeStereo {
public:
    OFR_PredictorStereo_Inner main;     // the +0x10 cross-channel LDLT predictor
    std::vector<OFR_CascadeRing> ringsA, ringsB;  // per-stage shared error rings (1-based)
    OFR_SubCascade casL, casR;          // L: primary A, secondary B ; R: primary B, secondary A

    int min_L=0, max_L=0, min_R=0, max_R=0, shift=0;
    int total_samples=0;
    OFR_StereoLpcParams main_params;

    bool cascade_only = false;          // pred_type=2
    int seg_len=0, cc_count=0, mode_L=0, mode_R=0; uint32_t sched_idx=0;
    std::vector<uint8_t> schedL, schedR;
    bool need_init=false;

    void init(OFR_RangeCoder* rc, int Lmn,int Lmx,int Rmn,int Rmx, int dbits, int total);
    void init_cascade_only(OFR_RangeCoder* rc, int Lmn,int Lmx,int Rmn,int Rmx, int dbits);
    void decode(int32_t* dest, uint32_t count);
    void setup_for_encode(int Lmn,int Lmx,int Rmn,int Rmx, int dbits, int total,
                          uint32_t main_w, uint32_t main_iv, uint32_t main_maxord, uint32_t main_rightord,
                          int n_stages, double decay_, uint32_t fcw, uint32_t fch_k, uint32_t golomb_field,
                          const std::vector<int>& stage_size1, const std::vector<int>& stage_size2,
                          const std::vector<int>& stage_mu10,
                          const std::vector<uint8_t>& schL, const std::vector<uint8_t>& schR);
    void cascade_init();
    int  sub_predict(OFR_SubCascade& c, std::vector<OFR_CascadeRing>& pri, std::vector<OFR_CascadeRing>& sec);
    void sub_update(OFR_SubCascade& c, std::vector<OFR_CascadeRing>& pri, std::vector<OFR_CascadeRing>& sec, double actual);
    void fc_solve(OFR_SubCascade& c);

private:
    void read_cascade_params(OFR_RangeCoder* rc);
};

// entropy_type=3: "acm" advanced context-modeling entropy (preset max). All integer.
// See doc/ent3_analysis.md.
class OFR_EntropyAcm {
public:
    struct Ctx { std::vector<uint32_t> freqs; uint32_t num_symbols=0, total_freq=0, limit=0, num_nodes=0; };
    struct Chan {
        bool msb_flag=false;
        uint32_t reset=0, state=0, countdown=0;
        int64_t hist[8] = {0,0,0,0,0,0,0,0};
        std::vector<Ctx> bitlen;            // +0x58: 9 ctxs of 9 symbols
        // fixed tables of 32, as in the reference. a block only rewrites the entries below its
        // bit depth, so a damaged stream can reach entries left over from earlier blocks
        std::array<Ctx, 32> vctx;           // +0x70: value contexts
        std::array<int, 32> vmap{};         // +0x370: exponent -> ctx index
        std::array<uint32_t, 32> k{};       // +0x3f0
        std::array<uint32_t, 32> nsym{};    // +0x470
        std::array<uint32_t, 32> transform{};   // +0x4f0
        std::array<uint32_t, 32> scale{};   // +0x570
    };
    uint32_t bit_depth=0, channels=0;
    std::vector<Chan> chans;
    void init(OFR_RangeCoder* rc, uint32_t bd, uint32_t ch);
    void decode_block(int32_t* dest, uint32_t count, OFR_RangeCoder* rc);
    uint32_t decode_sample(Chan& c, OFR_RangeCoder* rc);
};

class OFR_PostProcessor {
public:
    void init(OFR_RangeCoder* rc, uint32_t bit_depth, uint32_t channels, bool clamp);
    void init2(OFR_RangeCoder* rc, uint32_t channels, uint32_t bit_depth);   // post_type=2

    uint32_t m_channels = 1;

    int min_val_L = -0x800000, max_val_L = 0x7fffff, mult_L = 1, offset_L = 0;
    int min_val_R = -0x800000, max_val_R = 0x7fffff, mult_R = 1, offset_R = 0;

    // range the predictor and the entropy coder work in, after dividing out mult and offset
    int scaled_min_L = -0x800000, scaled_max_L = 0x7fffff;
    int scaled_min_R = -0x800000, scaled_max_R = 0x7fffff;

    // pred_type=4 without a correction file: quantization can legitimately overshoot the stored
    // min/max, so output is clamped to the full sample range instead, and identity blocks are
    // clamped too (FUN_00016f10 / FUN_000170e0)
    bool full_range_clamp = false;
    bool wide = false;   // reconstruct in 64 bits, see ofr_reduce_range

    // post_type=2 value-remap (tonal signals): per-channel dense-index -> original-value tables
    bool has_remap = false;
    std::vector<int32_t> remap_tbl[2];   // indexed by (decoded_value - remap_lo[ch])
    int remap_lo[2] = {0, 0};

    void decode(int32_t* dest, uint32_t count);

private:
    void read_channel(OFR_RangeCoder* rc, uint32_t bit_depth, int& mn, int& mx, int& mult, int& offset);
};


// the three stages of a block. objects are cached across blocks, and only the ones selected by the
// current block run: blocks are free to switch predictor or entropy coder.
class OFR_BlockDecoder {
public:
    void init_block(OFR_RangeCoder& rc, uint32_t reserved, uint32_t channels, uint32_t bits, uint32_t values);
    void decode_block(int32_t* dest, uint32_t count, OFR_RangeCoder& rc);

private:
    uint32_t post_type_ = 0, pred_type_ = 0, entropy_type_ = 0, channels_ = 1;

    std::unique_ptr<OFR_EntropyDecoder> entropy_;
    std::unique_ptr<OFR_EntropyAcm> entropy_acm_;
    std::unique_ptr<OFR_PostProcessor> post_;
    std::unique_ptr<OFR_Predictor> predictor_;
    std::unique_ptr<OFR_PredictorStereo> predictor_stereo_;
    std::unique_ptr<OFR_PredictorCascadeMono> cascade_mono_;
    std::unique_ptr<OFR_PredictorCascadeStereo> cascade_stereo_;
    std::unique_ptr<OFR_PredictorDualStreamMono> dualstream_mono_;
    std::unique_ptr<OFR_PredictorDualStreamStereo> dualstream_stereo_;
};

// the "DETA" sub-block following each COMP block of an IEEE Float file: what's needed to rebuild
// the original float32 bit patterns from the integers of the block.
class OFR_FloatBlock {
public:
    void init(std::vector<uint8_t>&& payload);
    void reconstruct(int32_t* buf, uint32_t count);

private:
    std::vector<uint8_t> payload_;
    OFR_RangeCoder rc_;
    OFR_ModelContext ctx2_, ctx279_;
    int32_t exp_range_ = 0, sig_shift_ = 0, min_exp_ = 0, max_exp_ = 0;
};


// .ofr/.off/.ofs stream decoder. positions and counts are in values (frames * channels), as in
// the container. open(), read(), seek() and read_tail() throw OFR_StreamError for what can't be
// recovered from; damage confined to a block silences that block and sets has_recoverable_errors.
class OFR_DecoderEngine {
public:
    OFR_DecoderEngine(ReadInterface* rInt, void* readerInstance);
    ~OFR_DecoderEngine();
    OFR_DecoderEngine(const OFR_DecoderEngine&) = delete;
    OFR_DecoderEngine& operator=(const OFR_DecoderEngine&) = delete;

    void open();
    // a fatal error after some values were decoded returns those values, unless keep_partial is
    // false: then it propagates, as the official library does for a whole OptimFROG_read request
    uint32_t read(int32_t* dest, uint32_t values, bool keep_partial = true);
    bool seek(uint64_t value_pos);
    void read_tail();
    void close();

    OFR_BitStream& stream() { return bs_; }

    uint64_t total_samples = 0;
    uint32_t sample_type = 0;
    uint32_t channel_config = 0;
    uint32_t samplerate = 0;
    uint32_t version = 0;
    uint32_t method = 0;
    uint32_t speedup = 0;
    uint32_t channels = 0;
    uint32_t bitspersample = 0;
    bool is_float = false;
    bool has_recoverable_errors = false;
    uint64_t position = 0;

    std::vector<uint8_t> head;
    std::vector<uint8_t> tail;

private:
    struct IndexEntry { uint64_t offset; uint64_t value_pos; uint32_t values; };

    void next_block();
    void remember_block(uint64_t offset, uint64_t value_pos, uint32_t values);
    bool hop_to_block(uint64_t value_pos);

    OFR_BitStream bs_;
    OFR_BlockDecoder block_decoder_;
    OFR_FloatBlock float_block_;
    OFR_RangeCoder range_coder_;
    std::vector<uint8_t> payload_;

    uint32_t block_values_ = 0;
    uint32_t block_pos_ = 0;
    bool need_new_block_ = true;
    bool block_error_ = false;
    uint64_t first_block_pos_ = 0;
    uint64_t next_block_pos_ = 0;
    std::vector<IndexEntry> index_;
    std::vector<int32_t> scratch_;
};

// ID3v1 and APEv2 tags at the end of the stream, the APE ones taking precedence on a shared key
using OFR_TagList = std::vector<std::pair<std::string, std::string>>;
OFR_TagList ofr_read_tags(OFR_BitStream& bs);

// the figures OptimFROG_getInfo reports beyond the header fields, computed the reference's way
struct OFR_DerivedInfo {
    int64_t noPoints = 0, originalSize = 0, compressedSize = 0, length_ms = 0;
    uint32_t bitrate = 0;
};
OFR_DerivedInfo ofr_derived_info(OFR_DecoderEngine& e);

// name lookups for the numeric header fields; out of range values give "unknown"
const char* ofr_method_name(uint32_t method);
const char* ofr_speedup_name(uint32_t speedup);
const char* ofr_sample_type_name(uint32_t sample_type);
const char* ofr_channel_config_name(uint32_t channel_config);

#endif
