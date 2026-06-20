# OptimFROG Internal Algorithms

This document preserves the knowledge gathered during the reverse engineering of the OptimFROG audio codec.

## 1. Bitstream and Block Header
OptimFROG splits the audio into blocks. The bitstream is read sequentially using a custom bit-reader wrapper.
Each block starts with a header that configures the compression parameters for that block:
- **uVar8** (8-bit): Contains flags. `(uVar8 >> 3) & 1` indicates if the Fast Stereo Predictor is used.
- **bVar2** (8-bit): More flags. If `bVar2 & 1` is true, the predictor order is smaller.
- **uVar15** (16-bit): A critical coefficient used throughout entropy decoding (usually 32). It drives the exponential weights for probabilities.

## 2. Entropy Decoding (Range Coder)
OptimFROG uses a custom Range Coder to decode symbols.
It maintains:
- `value` (uint32_t)
- `range` (uint32_t)
- `cache` (uint8_t)
- `pos` (position in bitstream)

### `decode_one_sample`
The entropy decoder takes a `variance` reference as input. The variance is mapped to an exponentially scaled probability index.
It updates the `variance` after each symbol read, adjusting it dynamically based on the decoded value and the initial `uVar15` (often called `iVar9` or `weight` internally).
The variance update formula looks like:
`variance = variance * weight1 + (decoded_val * decoded_val) * weight2`

## 3. Predictors
There are three main predictors:
1. `OFR_PredictorMono`
2. `OFR_PredictorStereo_Fast`
3. `OFR_PredictorStereo_Generic`

### Fast Stereo Predictor (`OFR_PredictorStereo_Inner`)
This is the most highly optimized path (used in Fast/Normal presets for 16-bit audio).
The channels are processed in pairs, but the matrices are updated using a recursive least squares (RLS) like algorithm.

#### History and R-Arrays
The predictor maintains a history of past samples (`m_left_hist_ptr`, `m_right_hist_ptr`) and a set of autocorrelation/cross-correlation coefficients (`m_R_left`, `m_R_right`).
When processing a block, it occasionally updates its internal matrices using a Cholesky LDLT decomposition.

#### The LDLT Solver Trick (Divergence Fix)
The custom Cholesky solver (`OFR_SolveLDLT` at `FUN_00117460`) is highly unorthodox:
1. It takes three pointers: `matrix`, `out_sol`, `diagonal`.
2. `matrix` is a fully populated lower triangular matrix.
3. **The trick:** The solver does *not* take a separate Right-Hand Side (RHS) vector. Instead, the caller must initialize the `out_sol` array with the initial RHS vector *before* calling the solver.
4. The solver mutates the `out_sol` array in place through forward and backward substitution.
5. `diagonal` is just a temporary scratchpad array provided by the caller to hold the `D` values of the LDLT decomposition.

**Caller Setup (C++ Equivalent):**
```cpp
// Initialize RHS
for (int i = 0; i < order; i++) {
    m_left_coefs[i] = m_R_left[i * 2 + 2];
}
// Solve in place
OFR_SolveLDLT(&m_temp_matrix[0][0], m_left_coefs, m_temp_vector, m_max_order);
```

If `m_temp_vector` is mistakenly used as the RHS, the solver will read uninitialized memory (or stale coefficients) from `m_left_coefs`, causing the audio to slowly diverge starting around sample 274.

## 4. Post-Processor
The decoded samples from the predictor are passed through a post-processor before becoming raw PCM bytes.
The post-processor applies scaling and offsets to correctly map the internal representation to the final bit depth.

For 16-bit audio:
- The initial values often use `mult = 1` and `offset = 0`, meaning it's a straight passthrough.
- However, if the Range Coder detects uniform splits, it adjusts `mult` and `offset` which must be propagated to the post-processor to reconstruct the correct PCM value.
