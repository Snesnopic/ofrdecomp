// minimal MD5 (RFC 1321), used by the ofr CLI's --md5/--check flags to store/verify
// the MD5 signature block of the original raw PCM input data.
#ifndef OFR_MD5_H_INCLUDED
#define OFR_MD5_H_INCLUDED

#include <cstdint>
#include <cstring>
#include <cstddef>

struct MD5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t total_len = 0;
    uint8_t buf[64];
    size_t buf_len = 0;

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void process_block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) | ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3*i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rotl(F, S[i]);
        }
        a += A; b += B; c += C; d += D;
    }

    void update(const uint8_t* data, size_t len) {
        total_len += len;
        while (len > 0) {
            size_t take = 64 - buf_len;
            if (take > len) take = len;
            std::memcpy(buf + buf_len, data, take);
            buf_len += take; data += take; len -= take;
            if (buf_len == 64) { process_block(buf); buf_len = 0; }
        }
    }

    void finish(uint8_t out[16]) {
        uint64_t bit_len = total_len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buf_len != 56) update(&zero, 1);
        uint8_t lenbytes[8];
        for (int i = 0; i < 8; i++) lenbytes[i] = (uint8_t)(bit_len >> (8*i));
        // bypass update()'s total_len accounting for the length field itself
        std::memcpy(buf + buf_len, lenbytes, 8);
        process_block(buf);
        uint32_t regs[4] = {a, b, c, d};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                out[i*4+j] = (uint8_t)(regs[i] >> (8*j));
    }
};

inline void md5_compute(const uint8_t* data, size_t len, uint8_t out[16]) {
    MD5 m; m.update(data, len); m.finish(out);
}

#endif
