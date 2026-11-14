#include "../../include/optimfrog_tables.h"
#include "../../include/optimfrog_decoder.h"
#include <algorithm>
#include <cmath>

void OFR_Predictor::init(int ord, int interval, double damp) {

    order = ord;
    update_interval = interval;
    damping = damp;
    sample_count = 0;
    last_update_count = 0;
    fail_count = 0;
    
    history_size = std::max(1024, order * 2);
    history_buffer.assign((size_t)history_size + 4, 0.0);
    vector_R.assign(order + 1, 0.0);
    weights.assign(order + 4, 0.0);
    if (order > 0) weights[0] = 1.0;  // binary: FUN_00014dc0 initializes weights[0]=1.0
    temp_D.assign(order + 1, 0.0);
    inst_matrix.assign(order + 1, std::vector<double>(order + 1, 0.0));
    cov_matrix.assign(order + 1, std::vector<double>(order + 1, 0.0));
    
    history_head = history_buffer.data() + history_size - order;
}

void OFR_Predictor::init(OFR_RangeCoder* rc) {
    uint32_t w = rc->read_uniform_bits(12);
    int weight_param = (w == 0xfff) ? (int)rc->read_uniform_split(16) + 0x1001 : (int)w + 2;

    uint32_t iv = rc->read_uniform_bits(3);
    int interval = (iv == 7) ? (int)rc->read_uniform_split(16) + 1 : DAT_00326238[iv];

    uint32_t od = rc->read_uniform_bits(5);
    int ord = (od != 31) ? DAT_00326220[od] : (int)rc->read_uniform_bits(8) + 1;
    if (ord < 1 || ord > OFR_MAX_ORDER) throw OFR_StreamError("invalid predictor order");

    this->init(ord, interval, ((double)weight_param - 1.0) / (double)weight_param);
}

// FUN_00014ce0: SSE dot product, pairs joined with haddpd into two running sums. at 32 bits and
// high orders a plain sequential sum rounds differently often enough to change the prediction
double OFR_Predictor::predict() {
    double a0 = 0.0, a1 = 0.0;
    const double* h = history_head;
    const double* w = weights.data();
    for (int n = 0; n < order; n += 4) {
        a0 = (h[n] * w[n] + h[n + 1] * w[n + 1]) + a0;
        a1 = (h[n + 2] * w[n + 2] + h[n + 3] * w[n + 3]) + a1;
    }
    return a1 + a0;
}

void OFR_Predictor::update_cov_1() {
    double head_val = history_head[0];
    for (int i = 0; i <= order; i++) {
        vector_R[i] = vector_R[i] * damping + history_head[i] * head_val;
    }
}

bool OFR_Predictor::solve_ldlt() {
    // faithful port of FUN_00012ca0 (in-place LDLT, stride-128, threshold 2^-17, exact float op order)
    const double thr = 7.62939453125e-06; // 2^-17
    double d0 = cov_matrix[0][0];
    temp_D[0] = d0;
    if (std::abs(d0) < thr) return false;
    cov_matrix[0][0] = 1.0;
    for (int i = 1; i < order; i++) {
        for (int j = 0; j < i; j++) {
            double v = cov_matrix[i][j];
            for (int k = 0; k < j; k++) v -= cov_matrix[i][k] * temp_D[k] * cov_matrix[j][k];
            cov_matrix[i][j] = v / temp_D[j];
        }
        double d = cov_matrix[i][i];
        cov_matrix[i][i] = 1.0;
        for (int k = 0; k < i; k++) d -= cov_matrix[i][k] * cov_matrix[i][k] * temp_D[k];
        temp_D[i] = d;
        if (std::abs(d) < thr) return false;
    }
    // forward substitution
    for (int i = 1; i < order; i++) {
        double v = weights[i];
        for (int k = 0; k < i; k++) v -= cov_matrix[i][k] * weights[k];
        weights[i] = v;
    }
    // back substitution
    weights[order - 1] = weights[order - 1] / temp_D[order - 1];
    for (int i = order - 2; i >= 0; i--) {
        double v = weights[i] / temp_D[i];
        for (int k = i + 1; k < order; k++) v -= cov_matrix[k][i] * weights[k];
        weights[i] = v;
    }
    return true;
}

void OFR_Predictor::update_weights() {
    double vR0 = vector_R[0];
    if (vR0 >= 0.5) {   // DAT_19770 energy gate (FUN_00014e30)
        double damping_inv = 1.0 / damping;

        if (sample_count < 48000) {
            // damp_pow = damping^(sample_count-order-1) via exp-by-squaring (matches FUN_00014e30 exactly)
            uint32_t e = sample_count - (uint32_t)order - 1u;
            double base = damping, damp_pow = 1.0;
            while (e != 0) { if (e & 1u) damp_pow *= base; base *= base; e >>= 1; }
            
            if (order > 0) {
                for (int i = 0; i < order; i++) {
                    cov_matrix[i][0] = (vector_R[i] - history_head[i] * history_head[0]) * damping_inv + inst_matrix[i][0] * damp_pow;
                    for (int j = 0; j < i; j++) {
                        cov_matrix[i][j+1] = (cov_matrix[i-1][j] - history_head[i] * history_head[j+1]) * damping_inv + inst_matrix[i][j+1] * damp_pow;
                    }
                }
            }
        } else {
            if (order > 0) {
                cov_matrix[0][0] = (vector_R[0] - history_head[0] * history_head[0]) * damping_inv;
                for (int i = 1; i < order; i++) {
                    cov_matrix[i][0] = (vector_R[i] - history_head[i] * history_head[0]) * damping_inv;
                    for (int j = 0; j < i; j++) {
                        cov_matrix[i][j+1] = (cov_matrix[i-1][j] - history_head[i] * history_head[j+1]) * damping_inv;
                    }
                }
            }
        }
        
        for (int i = 0; i < order; i++) {
            weights[i] = vector_R[i+1];
        }
        
        bool success = solve_ldlt();
        if (!success) {
            fail_count++;
            if (order > 0) {
                for (int i = 0; i < order; i++) weights[i] = 0.0;
                
                int iVar4 = 0;
                if (vector_R[0] != vector_R[1]) {
                    int iVar3 = 1;
                    while (iVar3 < order && vector_R[0] != vector_R[iVar3 + 1]) {
                        iVar3++;
                    }
                    iVar4 = iVar3;
                }
                if (iVar4 < order) {
                    weights[iVar4] = 1.0;
                } else {
                    weights[0] = 1.0;
                }
            }
        }
    } else {
        if (order > 0) weights[0] = 1.0;
        for (int i = 1; i < order; i++) weights[i] = 0.0;
    }
}

void OFR_Predictor::update(double sample) {
    if (!history_head) {
        return;
    }

    if (history_head == history_buffer.data()) {
        if (order > 0) {
            int j = history_size - order;
            for (int i = 0; i < order; i++) {
                history_buffer[j++] = history_head[i];
            }
        }
        history_head = history_buffer.data() + history_size - order;
    }
    history_head--;
    history_head[0] = sample;

    sample_count++;
    
    if (sample_count >= (uint32_t)(order + 1)) {
        if (sample_count == (uint32_t)(order + 1)) {
            if (order > 0) {
                for (int i = 0; i < order; i++) {
                    for (int j = 0; j <= i; j++) {
                        inst_matrix[i][j] = history_head[j+1] * history_head[i+1];
                    }
                }
            }
            last_update_count = sample_count;
        }
        
        update_cov_1();
        
        if ((uint32_t)update_interval <= sample_count - last_update_count) {
            if (vector_R[0] < 1e-10 && vector_R[0] != 0.0) {
                for (int i = 0; i <= order; i++) vector_R[i] = 0.0;
            }
            update_weights();
            last_update_count = sample_count;
        }
    }
}

void OFR_Predictor::decode(int32_t* dest, uint32_t count) {
    int sh = shift & 0x1f;
    for (uint32_t i = 0; i < count; i++) {
        int error = dest[i];
        int p = round_to_int32_cvtsd2si(predict());
        int cp = std::max(min_val, std::min(p, max_val));
        int v = ofr_wrap_add(cp, error, sh);
        if (v < min_val || v > max_val) throw OFR_StreamError("reconstructed sample out of range");
        dest[i] = v;
        update((double)v);
    }
}

static inline int32_t post1_sample(int32_t v, bool wide, int mult, int offset, int mn, int mx) {
    if (wide) {
        int64_t s = (int64_t)(uint32_t)mult * (int64_t)v + offset;
        if (s <= mn) return mn;
        return s > mx ? mx : (int32_t)s;
    }
    int32_t s = (int32_t)((uint32_t)mult * (uint32_t)v + (uint32_t)offset);
    if (s <= mn) return mn;
    return s > mx ? mx : s;
}

// FUN_000170e0 / FUN_00017700. identity channels are left alone unless the block asks for the
// full range clamp, everything else is scaled back and clamped.
void OFR_PostProcessor::decode(int32_t* dest, uint32_t count) {
    if (has_remap) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t ch = i % m_channels;
            int64_t idx = (int64_t)dest[i] - remap_lo[ch];
            if (idx < 0 || idx >= (int64_t)remap_tbl[ch].size()) throw OFR_StreamError("value outside the remap table");
            dest[i] = remap_tbl[ch][(size_t)idx];
        }
        return;
    }

    const bool active_L = mult_L != 1 || offset_L != 0 || full_range_clamp;
    const bool active_R = mult_R != 1 || offset_R != 0 || full_range_clamp;
    if (m_channels == 1) {
        if (!active_L) return;
        for (uint32_t i = 0; i < count; i++)
            dest[i] = post1_sample(dest[i], wide, mult_L, offset_L, min_val_L, max_val_L);
        return;
    }
    for (uint32_t i = 0; i + 1 < count; i += 2) {
        if (active_L) dest[i] = post1_sample(dest[i], wide, mult_L, offset_L, min_val_L, max_val_L);
        if (active_R) dest[i + 1] = post1_sample(dest[i + 1], wide, mult_R, offset_R, min_val_R, max_val_R);
    }
}

void OFR_PredictorStereo_Inner::init(double weight, int max_order, int left_order, uint32_t update_interval) {
    std::memset(this, 0, sizeof(*this));
    m_max_order = max_order;
    m_left_order = left_order;
    m_right_order = max_order - left_order;
    m_update_interval = update_interval;
    m_weight = weight;
    m_left_hist_ptr = &m_left_history[HISTORY - left_order];
    m_right_hist_ptr = &m_right_history[HISTORY - left_order];
    m_left_coefs[0] = 1.0;
    m_right_coefs[0] = 1.0;
}

// FUN_00012ed0/00012f90: 2-accumulator pairwise FIR (4 taps/iter: a6<-taps0,1 a7<-taps2,3, each
// pair grouped (h0*c0+h1*c1)+acc), return a7+a6. Float op-order matters — a sequential single-acc
// sum diverges 1 ULP and lrint() then rounds to ±1 on borderline predictions. All pred=1 stereo
// orders are multiples of 4 (DAT tables) → no padding/over-read.
double OFR_PredictorStereo_Inner::predictLeft() {
    double a6 = 0.0, a7 = 0.0;
    const double* h = m_left_hist_ptr; const double* c = m_left_coefs;
    for (int n = 0; n < m_left_order; n += 4) {
        a6 = (h[n]*c[n] + h[n+1]*c[n+1]) + a6;
        a7 = (h[n+2]*c[n+2] + h[n+3]*c[n+3]) + a7;
    }
    h = m_right_hist_ptr; c = m_left_coefs + m_left_order;
    for (int n = 0; n < m_right_order; n += 4) {
        a6 = (h[n]*c[n] + h[n+1]*c[n+1]) + a6;
        a7 = (h[n+2]*c[n+2] + h[n+3]*c[n+3]) + a7;
    }
    return a7 + a6;
}

double OFR_PredictorStereo_Inner::predictRight() {
    double a6 = 0.0, a7 = 0.0;
    const double* h = m_right_hist_ptr; const double* c = m_right_coefs;
    for (int n = 0; n < m_left_order; n += 4) {   // first block count = left_order
        a6 = (h[n]*c[n] + h[n+1]*c[n+1]) + a6;
        a7 = (h[n+2]*c[n+2] + h[n+3]*c[n+3]) + a7;
    }
    h = m_left_hist_ptr; c = m_right_coefs + m_left_order;
    for (int n = 0; n < m_right_order; n += 4) {
        a6 = (h[n]*c[n] + h[n+1]*c[n+1]) + a6;
        a7 = (h[n+2]*c[n+2] + h[n+3]*c[n+3]) + a7;
    }
    return a7 + a6;
}

void OFR_PredictorStereo_Inner::updateAutocorrLeft() {
    double w = m_weight;
    double hl0 = m_left_hist_ptr[0];
    double hr0 = m_right_hist_ptr[0];
    double* R = m_R_left;
    double* h_ptr = m_left_hist_ptr + 1;
    double h_prev = hl0;
    
    int left_order = m_left_order;
    while (left_order > 0) {
        double h_curr = h_ptr[0];
        R[0] = R[0] * w + h_prev * hl0;
        R[1] = R[1] * w + h_curr * hr0;
        h_prev = h_ptr[1];
        R[2] = R[2] * w + h_curr * hl0;
        R[3] = R[3] * w + h_prev * hr0;
        R += 4;
        h_ptr += 2;
        left_order -= 2;
    }
    
    int right_order = m_right_order;
    h_ptr = m_right_hist_ptr;
    while (right_order > 0) {
        double h_curr = h_ptr[0];
        R[0] = R[0] * w + h_prev * hl0;
        R[1] = R[1] * w + h_curr * hr0;
        h_prev = h_ptr[1];
        R[2] = R[2] * w + h_curr * hl0;
        R[3] = R[3] * w + h_prev * hr0;
        R += 4;
        h_ptr += 2;
        right_order -= 2;
    }
    R[0] = R[0] * w + h_prev * hl0;
}

void OFR_PredictorStereo_Inner::updateAutocorrRight() {
    double w = m_weight;
    double hr0 = m_right_hist_ptr[0];
    double hl0 = m_left_hist_ptr[0];
    double* R = m_R_right;
    double* h_ptr = m_right_hist_ptr + 1;
    double h_prev = hr0;
    
    int left_order = m_left_order;
    while (left_order > 0) {
        double h_curr = h_ptr[0];
        R[0] = R[0] * w + h_prev * hr0;
        R[1] = R[1] * w + h_curr * hl0;
        h_prev = h_ptr[1];
        R[2] = R[2] * w + h_curr * hr0;
        R[3] = R[3] * w + h_prev * hl0;
        R += 4;
        h_ptr += 2;
        left_order -= 2;
    }
    
    int right_order = m_right_order;
    h_ptr = m_left_hist_ptr;
    while (right_order > 0) {
        double h_curr = h_ptr[0];
        R[0] = R[0] * w + h_prev * hr0;
        R[1] = R[1] * w + h_curr * hl0;
        h_prev = h_ptr[1];
        R[2] = R[2] * w + h_curr * hr0;
        R[3] = R[3] * w + h_prev * hl0;
        R += 4;
        h_ptr += 2;
        right_order -= 2;
    }
    R[0] = R[0] * w + h_prev * hr0;
}

void OFR_PredictorStereo::decode(int32_t* outputs, uint32_t num_samples) {
    if (m_need_init) {
        double w = (static_cast<double>(m_params.weight_param) - 1.0) / static_cast<double>(m_params.weight_param);
        m_inner.init(w, m_params.max_order, m_params.max_order - m_params.right_order, m_params.update_interval);
        m_need_init = false;
    }

    int shift = (int)(m_shift & 0x1f);
    for (uint32_t i = 0; i < num_samples; i += 2) {
        int32_t res = outputs[i];
        int32_t pr  = round_to_int32_cvtsd2si(m_inner.predictLeft());
        int32_t cl  = std::max(m_left_min, std::min(pr, m_left_max));
        int32_t v   = ofr_wrap_add(cl, res, shift);
        if (v < m_left_min || v > m_left_max) throw OFR_StreamError("reconstructed sample out of range");
        outputs[i] = v;
        m_inner.updateLeft((double)v);

        res = outputs[i + 1];
        pr  = round_to_int32_cvtsd2si(m_inner.predictRight());
        int32_t cr = std::max(m_right_min, std::min(pr, m_right_max));
        v = ofr_wrap_add(cr, res, shift);
        if (v < m_right_min || v > m_right_max) throw OFR_StreamError("reconstructed sample out of range");
        outputs[i + 1] = v;
        m_inner.updateRight((double)v);
    }
}

static bool OFR_SolveLDLT(double* matrix, double* rhs_out, double* diagonal, int size) {
    uint64_t abs_mask = 0x7FFFFFFFFFFFFFFF;
    double d_val = matrix[0];
    diagonal[0] = d_val;
    
    union { double d; uint64_t u; } uval;
    uval.d = d_val;
    uval.u &= abs_mask;
    if (uval.d < 7.62939453125e-06) {
        return false;
    }
    
    matrix[0] = 1.0;
    if (size > 1) {
        for (int i = 1; i < size; ++i) {
            double* row_i = matrix + i * 128;
            double d_val_i = row_i[i];
            
            for (int j = 0; j < i; ++j) {
                double val = row_i[j];
                if (j > 0) {
                    for (int k = 0; k < j; ++k) {
                        val -= row_i[k] * diagonal[k] * matrix[j * 128 + k];
                    }
                }
                row_i[j] = val / diagonal[j];
            }
            
            d_val_i = matrix[i * 128 + i];
            matrix[i * 128 + i] = 1.0;
            for (int k = 0; k < i; ++k) {
                d_val_i -= row_i[k] * row_i[k] * diagonal[k];
            }
            diagonal[i] = d_val_i;
            
            uval.d = d_val_i;
            uval.u &= abs_mask;
            if (uval.d < 7.62939453125e-06) {
                return false;
            }
        }
        
        for (int i = 1; i < size; ++i) {
            double val = rhs_out[i];
            for (int k = 0; k < i; ++k) {
                val -= matrix[i * 128 + k] * rhs_out[k];
            }
            rhs_out[i] = val;
        }
    }
    
    rhs_out[size - 1] = rhs_out[size - 1] / diagonal[size - 1];
    for (int i = size - 2; i >= 0; --i) {
        double val = rhs_out[i] / diagonal[i];
        for (int k = i + 1; k < size; ++k) {
            val -= matrix[k * 128 + i] * rhs_out[k];
        }
        rhs_out[i] = val;
    }
    
    return true;
}

void OFR_PredictorStereo_Inner::solveCholeskyLeft() {
    double r0 = m_R_left[0];
    if (r0 < 0.5) {
        m_left_coefs[0] = 1.0;
        if (m_max_order > 1) {
            for (int i = 1; i < m_max_order; ++i) {
                m_left_coefs[i] = 0.0;
            }
        }
        return;
    }
    
    double inv_w = 1.0 / m_weight;
    int left_order = m_left_order;
    
    if (m_count_left < 48000) {
        double dVar19 = 1.0;
        {
            double base = m_weight;
            for (uint32_t e = (uint32_t)((m_count_left - left_order) - 1); e != 0; e >>= 1) {
                if (e & 1) dVar19 *= base;
                base *= base;
            }
        }

        const double dp = dVar19, iw = inv_w;
        const int L = left_order, N = m_max_order;
        double* hl = m_left_hist_ptr;
        double* hr = m_right_hist_ptr;
        for (int i = 0; i < L; ++i) {
            m_temp_matrix[i][0] = m_left_matrix[i][0] * dp + (m_R_left[i*2] - hl[0]*hl[i]) * iw;
            for (int j = 1; j <= i; ++j)
                m_temp_matrix[i][j] = m_left_matrix[i][j] * dp + (m_temp_matrix[i-1][j-1] - hl[j]*hl[i]) * iw;
            m_left_coefs[i] = m_R_left[i*2+2];
            m_temp_matrix[L][i] = m_R_left[i*2+1];
        }
        for (int i = L; i < N; ++i) {
            if (i > L) {
                m_temp_matrix[i][0] = m_left_matrix[i][0] * dp + (m_R_left[i*2] - hl[0]*hr[(i-1)-L]) * iw;
                for (int j = 1; j < L; ++j)
                    m_temp_matrix[i][j] = m_left_matrix[i][j] * dp + (m_temp_matrix[i-1][j-1] - hl[j]*hr[(i-1)-L]) * iw;
            }
            m_temp_matrix[i][L] = m_R_left[i*2+1];
            for (int j = L+1; j <= i; ++j)
                m_temp_matrix[i][j] = m_left_matrix[i][j] * dp + (m_temp_matrix[i-1][j-1] - hr[(i-1)-L]*hr[(j-1)-L]) * iw;
            m_left_coefs[i] = m_R_left[i*2+2];
        }
    } else {
        const double iw = inv_w;
        const int L = left_order, N = m_max_order;
        double* hl = m_left_hist_ptr;
        double* hr = m_right_hist_ptr;
        for (int i = 0; i < L; ++i) {
            m_temp_matrix[i][0] = (m_R_left[i*2] - hl[0]*hl[i]) * iw;
            for (int j = 1; j <= i; ++j)
                m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hl[j]*hl[i]) * iw;
            m_left_coefs[i] = m_R_left[i*2+2];
            m_temp_matrix[L][i] = m_R_left[i*2+1];
        }
        for (int i = L; i < N; ++i) {
            if (i > L) {
                m_temp_matrix[i][0] = (m_R_left[i*2] - hl[0]*hr[(i-1)-L]) * iw;
                for (int j = 1; j < L; ++j)
                    m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hl[j]*hr[(i-1)-L]) * iw;
            }
            m_temp_matrix[i][L] = m_R_left[i*2+1];
            for (int j = L+1; j <= i; ++j)
                m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hr[(i-1)-L]*hr[(j-1)-L]) * iw;
            m_left_coefs[i] = m_R_left[i*2+2];
        }
    }

    if (!OFR_SolveLDLT(&m_temp_matrix[0][0], m_left_coefs, m_temp_vector, m_max_order)) {
        // two-stage singular fallback (FUN_00013310): zero coefs; a single tap with R[0]==R[2i+2];
        // else rebuild the left-only (mono) block of left_order and re-solve; else coef0=1.
        for (int i = 0; i < m_max_order; ++i) m_left_coefs[i] = 0.0;
        for (int i = 0; i < m_max_order; ++i)
            if (m_R_left[0] == m_R_left[2*i+2]) { m_left_coefs[i] = 1.0; return; }
        double iw = 1.0 / m_weight;
        double* hl = m_left_hist_ptr;
        int L = m_left_order;
        if (m_count_left < 48000) {
            for (int i = 0; i < L; ++i) {
                m_temp_matrix[i][0] = (m_R_left[i*2] - hl[0]*hl[i]) * iw + m_left_matrix[i][0];
                for (int j = 1; j <= i; ++j)
                    m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hl[j]*hl[i]) * iw + m_left_matrix[i][j];
                m_left_coefs[i] = m_R_left[i*2+2];
            }
        } else {
            for (int i = 0; i < L; ++i) {
                m_temp_matrix[i][0] = (m_R_left[i*2] - hl[0]*hl[i]) * iw;
                for (int j = 1; j <= i; ++j)
                    m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hl[j]*hl[i]) * iw;
                m_left_coefs[i] = m_R_left[i*2+2];
            }
        }
        if (!OFR_SolveLDLT(&m_temp_matrix[0][0], m_left_coefs, m_temp_vector, L)) {
            m_left_coefs[0] = 1.0;
            for (int i = 1; i < L; ++i) m_left_coefs[i] = 0.0;
        }
    }
}

void OFR_PredictorStereo_Inner::solveCholeskyRight() {
    double r0 = m_R_right[0];
    if (r0 < 0.5) {
        m_right_coefs[0] = 1.0;
        if (m_max_order > 1) {
            for (int i = 1; i < m_max_order; ++i) {
                m_right_coefs[i] = 0.0;
            }
        }
        return;
    }
    
    double inv_w = 1.0 / m_weight;
    int left_order = m_left_order;
    
    if (m_count_right < 48000) {
        double dVar19 = 1.0;
        {
            double base = m_weight;
            for (uint32_t e = (uint32_t)((m_count_right - left_order) - 1); e != 0; e >>= 1) {
                if (e & 1) dVar19 *= base;
                base *= base;
            }
        }

        const double dp = dVar19, iw = inv_w;
        const int L = left_order, N = m_max_order;
        double* hr = m_right_hist_ptr;
        double* hl = m_left_hist_ptr;
        for (int i = 0; i < L; ++i) {
            m_temp_matrix[i][0] = m_right_matrix[i][0] * dp + (m_R_right[i*2] - hr[0]*hr[i]) * iw;
            for (int j = 1; j <= i; ++j)
                m_temp_matrix[i][j] = m_right_matrix[i][j] * dp + (m_temp_matrix[i-1][j-1] - hr[j]*hr[i]) * iw;
            m_right_coefs[i] = m_R_right[i*2+2];
            m_temp_matrix[L][i] = m_R_right[i*2+1];
        }
        for (int i = L; i < N; ++i) {
            if (i > L) {
                m_temp_matrix[i][0] = m_right_matrix[i][0] * dp + (m_R_right[i*2] - hr[0]*hl[(i-1)-L]) * iw;
                for (int j = 1; j < L; ++j)
                    m_temp_matrix[i][j] = m_right_matrix[i][j] * dp + (m_temp_matrix[i-1][j-1] - hr[j]*hl[(i-1)-L]) * iw;
            }
            m_temp_matrix[i][L] = m_R_right[i*2+1];
            for (int j = L+1; j <= i; ++j)
                m_temp_matrix[i][j] = m_right_matrix[i][j] * dp + (m_temp_matrix[i-1][j-1] - hl[(i-1)-L]*hl[(j-1)-L]) * iw;
            m_right_coefs[i] = m_R_right[i*2+2];
        }
    } else {
        const double iw = inv_w;
        const int L = left_order, N = m_max_order;
        double* hr = m_right_hist_ptr;
        double* hl = m_left_hist_ptr;
        for (int i = 0; i < L; ++i) {
            m_temp_matrix[i][0] = (m_R_right[i*2] - hr[0]*hr[i]) * iw;
            for (int j = 1; j <= i; ++j)
                m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hr[j]*hr[i]) * iw;
            m_right_coefs[i] = m_R_right[i*2+2];
            m_temp_matrix[L][i] = m_R_right[i*2+1];
        }
        for (int i = L; i < N; ++i) {
            if (i > L) {
                m_temp_matrix[i][0] = (m_R_right[i*2] - hr[0]*hl[(i-1)-L]) * iw;
                for (int j = 1; j < L; ++j)
                    m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hr[j]*hl[(i-1)-L]) * iw;
            }
            m_temp_matrix[i][L] = m_R_right[i*2+1];
            for (int j = L+1; j <= i; ++j)
                m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hl[(i-1)-L]*hl[(j-1)-L]) * iw;
            m_right_coefs[i] = m_R_right[i*2+2];
        }
    }

    if (!OFR_SolveLDLT(&m_temp_matrix[0][0], m_right_coefs, m_temp_vector, m_max_order)) {
        // two-stage singular fallback (FUN_00013bf0): mirror of the left, using right history /
        // right autocorr, block size = left_order (the cross-channel primary block size).
        for (int i = 0; i < m_max_order; ++i) m_right_coefs[i] = 0.0;
        for (int i = 0; i < m_max_order; ++i)
            if (m_R_right[0] == m_R_right[2*i+2]) { m_right_coefs[i] = 1.0; return; }
        double iw = 1.0 / m_weight;
        double* hr = m_right_hist_ptr;
        int L = m_left_order;
        if (m_count_right < 48000) {
            for (int i = 0; i < L; ++i) {
                m_temp_matrix[i][0] = (m_R_right[i*2] - hr[0]*hr[i]) * iw + m_right_matrix[i][0];
                for (int j = 1; j <= i; ++j)
                    m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hr[j]*hr[i]) * iw + m_right_matrix[i][j];
                m_right_coefs[i] = m_R_right[i*2+2];
            }
        } else {
            for (int i = 0; i < L; ++i) {
                m_temp_matrix[i][0] = (m_R_right[i*2] - hr[0]*hr[i]) * iw;
                for (int j = 1; j <= i; ++j)
                    m_temp_matrix[i][j] = (m_temp_matrix[i-1][j-1] - hr[j]*hr[i]) * iw;
                m_right_coefs[i] = m_R_right[i*2+2];
            }
        }
        if (!OFR_SolveLDLT(&m_temp_matrix[0][0], m_right_coefs, m_temp_vector, L)) {
            m_right_coefs[0] = 1.0;
            for (int i = 1; i < L; ++i) m_right_coefs[i] = 0.0;
        }
    }
}

void OFR_PredictorStereo_Inner::updateLeft(double sample) {
    if (m_left_hist_ptr == &m_left_history[0]) {
        if (m_left_order > 0) {
            for (int i = 0; i < m_left_order; ++i) {
                m_left_history[HISTORY - m_left_order + i] = m_left_history[i];
            }
        }
        m_left_hist_ptr = &m_left_history[HISTORY - m_left_order];
    }
    
    m_left_hist_ptr--;
    m_count_left++;
    m_left_hist_ptr[0] = sample;
    
    if (m_count_left >= (uint32_t)(m_left_order + 1)) {
        if (m_count_left == (uint32_t)(m_left_order + 1)) {
            if (m_left_order > 0) {
                for (int i = 0; i < m_left_order; ++i) {
                    for (int j = 0; j <= i; ++j) {
                        m_left_matrix[i][j] = m_left_hist_ptr[j + 1] * m_left_hist_ptr[i + 1];
                    }
                }
            }
            if (m_left_order + 1 < m_max_order) {
                for (int i = m_left_order + 1; i < m_max_order; ++i) {
                    int right_idx = i - m_left_order;
                    if (m_left_order > 0) {
                        for (int j = 0; j < m_left_order; ++j) {
                            m_left_matrix[i][j] = m_right_hist_ptr[right_idx] * m_left_hist_ptr[j + 1];
                        }
                    }
                    for (int j = m_left_order + 1; j <= i; ++j) {
                        m_left_matrix[i][j] = m_right_hist_ptr[j - m_left_order] * m_right_hist_ptr[right_idx];
                    }
                }
            }
            m_update_count_left = m_count_left;
        }
        updateAutocorrLeft();
        if (m_update_interval <= m_count_left - m_update_count_left) {
            // DAT_19778 = 2^-30: snap a near-zero (numerical-noise) R[0] to exactly zero.
            // NOT a large-magnitude overflow guard -- that reading (an arbitrary 1e15 sentinel)
            // was wrong and, since real 16-bit R[0] never gets near it, was a silent no-op there;
            // at 24/32-bit R[0] legitimately exceeds 1e15, spuriously wiping valid correlation data.
            if (m_R_left[0] < 9.31322574615478515625e-10 && m_R_left[0] != 0.0) {
                std::memset(m_R_left, 0, (m_max_order * 2 + 1) * sizeof(double));
            }
            solveCholeskyLeft();
            m_update_count_left = m_count_left;
        }
    }
}

void OFR_PredictorStereo_Inner::updateRight(double sample) {
    if (m_right_hist_ptr == &m_right_history[0]) {
        if (m_left_order > 0) {
            for (int i = 0; i < m_left_order; ++i) {
                m_right_history[HISTORY - m_left_order + i] = m_right_history[i];
            }
        }
        m_right_hist_ptr = &m_right_history[HISTORY - m_left_order];
    }
    
    m_right_hist_ptr--;
    m_count_right++;
    m_right_hist_ptr[0] = sample;
    
    if (m_count_right >= (uint32_t)(m_left_order + 1)) {
        if (m_count_right == (uint32_t)(m_left_order + 1)) {
            if (m_left_order > 0) {
                for (int i = 0; i < m_left_order; ++i) {
                    for (int j = 0; j <= i; ++j) {
                        m_right_matrix[i][j] = m_right_hist_ptr[j + 1] * m_right_hist_ptr[i + 1];
                    }
                }
            }
            if (m_left_order + 1 < m_max_order) {
                for (int i = m_left_order + 1; i < m_max_order; ++i) {
                    int right_idx = i - m_left_order;
                    if (m_left_order > 0) {
                        for (int j = 0; j < m_left_order; ++j) {
                            m_right_matrix[i][j] = m_left_hist_ptr[right_idx] * m_right_hist_ptr[j + 1];
                        }
                    }
                    for (int j = m_left_order + 1; j <= i; ++j) {
                        m_right_matrix[i][j] = m_left_hist_ptr[j - m_left_order] * m_left_hist_ptr[right_idx];
                    }
                }
            }
            m_update_count_right = m_count_right;
        }
        updateAutocorrRight();
        if (m_update_interval <= m_count_right - m_update_count_right) {
            // DAT_19778 = 2^-30, same near-zero R clamp as updateLeft (see comment there).
            if (m_R_right[0] < 9.31322574615478515625e-10 && m_R_right[0] != 0.0) {
                std::memset(m_R_right, 0, (m_max_order * 2 + 1) * sizeof(double));
            }
            solveCholeskyRight();
            m_update_count_right = m_count_right;
        }
    }
}

void OFR_StereoLpcParams::read(OFR_RangeCoder* rc) {
    uint32_t w = rc->read_uniform_bits(12);
    weight_param = (w == 0xfff) ? rc->read_uniform_bits(16) + 0x1001 : w + 2;

    uint32_t iv = rc->read_uniform_bits(3);
    update_interval = (iv == 7) ? rc->read_uniform_bits(16) + 1 : (uint32_t)DAT_00326238[iv];

    uint32_t od = rc->read_uniform_bits(5);
    if (od != 31) {
        max_order = DAT_00326220[od];
        right_order = DAT_00326200[od];
    } else {
        max_order = (int32_t)rc->read_uniform_bits(8) + 1;
        right_order = (int32_t)rc->read_uniform_bits(8);
    }

    // the right history is rewound by left_order entries, so the right part can't be the larger one
    if (max_order < 1 || max_order > OFR_MAX_ORDER || right_order > max_order - right_order)
        throw OFR_StreamError("invalid stereo predictor order");
}

void OFR_PredictorStereo::init(OFR_RangeCoder* rc) {
    m_params.read(rc);
    m_need_init = true;
}
