// internal helpers shared by the decoder, the encoder and the CLIs.
#ifndef OFR_COMMON_H
#define OFR_COMMON_H

#include <array>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define OFR_HAVE_SSE2 1
#include <emmintrin.h>
#endif

// a bitstream the decoder cannot make sense of. caught per block, where it turns into silence plus
// the recoverable-error flag (the reference does the same with its own exception type).
struct OFR_StreamError : std::runtime_error {
    explicit OFR_StreamError(const std::string& what) : std::runtime_error(what) {}
};

// x86 cvtsd2si semantics, on every target: round to nearest-even, and INT32_MIN (the hardware
// "integer indefinite") for anything that doesn't fit, NaN included. the reference rounds every
// prediction this way, so reconstruction depends on it.
inline int32_t round_to_int32_cvtsd2si(double x) {
#if defined(OFR_HAVE_SSE2)
    return _mm_cvtsd_si32(_mm_set_sd(x));
#else
    double r = std::nearbyint(x);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (int32_t)r : INT32_MIN;
#endif
}

// x86 cvttsd2si semantics: truncate toward zero, same out-of-range sentinel. no intrinsic here:
// gcc 13 folds _mm_cvttsd_si32 on an out-of-range constant to a saturated value.
inline int32_t trunc_to_int32_cvttsd2si(double x) {
    return (x > -2147483649.0 && x < 2147483648.0) ? (int32_t)x : INT32_MIN;
}

inline int64_t trunc_to_int64_cvttsd2si(double x) {
    return (x >= -9223372036854775808.0 && x < 9223372036854775808.0) ? (int64_t)x : INT64_MIN;
}

// the shift-wrap trick truncates a value to data_bits; going through unsigned keeps the left
// shift defined for negative values.
inline int32_t shl_wrap(int32_t x, int sh) { return (int32_t)((uint32_t)x << sh); }

// a + b, wrapped to the 32-sh bits of the block's data range
inline int32_t ofr_wrap_add(int32_t a, int32_t b, int sh) { return shl_wrap((int32_t)((uint32_t)a + (uint32_t)b), sh) >> sh; }

// FUN_00018d80: bit length of the data range (min,max), which drives the entropy depth and the
// predictor shift.
inline int ofr_bitlen(int mn, int mx) {
    uint32_t a = (uint32_t)((mn >> 31) ^ mn);
    uint32_t b = (uint32_t)((mx >> 31) ^ mx);
    if (a == 0 && b == 0) return 1;
    uint32_t m = ((int)b <= (int)a) ? a : b;
    uint32_t u = (m < 0x10000u) ? m : (m >> 16);
    uint32_t bits = (m > 0xffffu) ? 16u : 0u;
    if (u > 0xffu) { u >>= 8; bits += 8; }
    if (u > 0xfu)  { u >>= 4; bits += 4; }
    if (u > 3u)    { u >>= 2; bits += 2; }
    return (int)(bits + 2u + (u > 1u ? 1u : 0u));
}

namespace ofr_detail {
constexpr std::array<uint32_t, 256> make_crc32_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
inline constexpr std::array<uint32_t, 256> crc32_table = make_crc32_table();
} // namespace ofr_detail

// zlib CRC32
inline uint32_t ofr_crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = ofr_detail::crc32_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// the reference pins MXCSR to its default (round to nearest, no flush-to-zero) on every entry
// point. the same applies here: a host with FTZ or another rounding mode must not change the output.
class OFR_ScopedFpEnv {
public:
    OFR_ScopedFpEnv() { std::fegetenv(&saved_); std::fesetenv(FE_DFL_ENV); }
    ~OFR_ScopedFpEnv() { std::fesetenv(&saved_); }
    OFR_ScopedFpEnv(const OFR_ScopedFpEnv&) = delete;
    OFR_ScopedFpEnv& operator=(const OFR_ScopedFpEnv&) = delete;
private:
    std::fenv_t saved_;
};

// four-byte block ids, little endian
constexpr uint32_t OFR_TAG_OFR  = 0x2052464f;   // "OFR "
constexpr uint32_t OFR_TAG_OFRX = 0x5852464f;   // "OFRX"
constexpr uint32_t OFR_TAG_HEAD = 0x44414548;   // "HEAD"
constexpr uint32_t OFR_TAG_COMP = 0x504d4f43;   // "COMP"
constexpr uint32_t OFR_TAG_DETA = 0x41544544;   // "DETA"
constexpr uint32_t OFR_TAG_TAIL = 0x4c494154;   // "TAIL"

// sample_type byte, as stored in the main header and in every COMP block
constexpr uint32_t OFR_SAMPLE_TYPE_COUNT = 11;
constexpr uint8_t OFR_SAMPLE_FLOAT32 = 8;
inline uint32_t ofr_sample_bits(uint32_t sample_type) {
    static constexpr uint8_t bits[OFR_SAMPLE_TYPE_COUNT] = {8, 8, 16, 16, 24, 24, 32, 32, 32, 32, 32};
    return bits[sample_type];
}

// predictor orders are capped here by the reference's fixed 128-row covariance matrices
constexpr int OFR_MAX_ORDER = 128;

#endif
