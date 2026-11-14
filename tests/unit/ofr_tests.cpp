// self-contained tests: everything here runs without the reference binaries.
#include "../../include/OptimFROG.h"
#include "../../include/ofrdecomp.hpp"
#include "../../include/optimfrog_encoder.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

// music-like: a few partials and some noise, scaled to the bit depth
std::vector<int32_t> make_signal(uint64_t frames, int channels, int bps, uint32_t seed = 1) {
    std::vector<int32_t> s((size_t)(frames * (uint64_t)channels));
    Rng rng(seed);
    const double scale = std::ldexp(0.4, bps - 1);
    for (uint64_t i = 0; i < frames; i++) {
        for (int c = 0; c < channels; c++) {
            double t = (double)i;
            double v = 0.6 * std::sin(t * 0.031 + c) + 0.3 * std::sin(t * 0.0071) + 0.05 * std::sin(t * 0.33 + 2.0 * c);
            s[(size_t)i * (size_t)channels + (size_t)c] = (int32_t)(v * scale) + (int32_t)(rng.next() % 17) - 8;
        }
    }
    return s;
}

std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("ofrdecomp_test_") + name)).string();
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    return data;
}

std::vector<int32_t> decode_all(const std::vector<uint8_t>& file) {
    ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
    return d.readAll();
}

std::vector<uint8_t> encode(const std::vector<int32_t>& s, int ch, int bps, const OFR_EncodeParams& p,
                            const std::vector<uint8_t>& head = {}, const std::vector<uint8_t>& tail = {}) {
    std::vector<uint8_t> file;
    bool ok = ofr_encode(s.data(), s.size() / (size_t)ch, ch, 44100, bps, p, file, head, tail);
    CHECK(ok);
    return file;
}

void test_range_coder_roundtrip() {
    enum Kind { UNIFORM, BITS, SPLIT, SYMBOL, GOLOMB };
    struct Op { Kind kind; uint32_t a, v; };
    const uint32_t NSYM = 37;
    Rng rng(1234);
    std::vector<Op> ops;
    for (int i = 0; i < 200000; i++) {
        switch (rng.next() % 5) {
        case 0: { uint32_t n = 2 + rng.next() % 60000; ops.push_back({UNIFORM, n, rng.next() % n}); break; }
        case 1: { uint32_t b = 1 + rng.next() % 20; ops.push_back({BITS, b, rng.next() & ((1u << b) - 1)}); break; }
        case 2: { uint32_t b = 1 + rng.next() % 24; ops.push_back({SPLIT, b, rng.next() & ((1u << b) - 1)}); break; }
        case 3: ops.push_back({SYMBOL, 0, rng.next() % NSYM}); break;
        default: ops.push_back({GOLOMB, 0, rng.next() % 2 ? 0 : 2 + rng.next() % 100000}); break;
        }
    }

    OFR_ModelContext enc_ctx, dec_ctx;
    enc_ctx.init(NSYM, 0x2000);
    dec_ctx.init(NSYM, 0x2000);
    OFR_RangeEncoder enc;
    for (const Op& op : ops) {
        switch (op.kind) {
        case UNIFORM: enc.encode_uniform(op.a, op.v); break;
        case BITS: enc.encode_bits(op.a, op.v); break;
        case SPLIT: enc.encode_split(op.a, op.v); break;
        case SYMBOL: enc.encode_symbol(enc_ctx, op.v); break;
        case GOLOMB: enc.encode_golomb(op.v); break;
        }
    }
    enc.flush();

    OFR_RangeCoder rc;
    rc.init(enc.out.data(), enc.out.size());
    int bad = 0;
    for (const Op& op : ops) {
        uint32_t v = 0;
        switch (op.kind) {
        case UNIFORM: v = rc.decode_uniform(op.a); break;
        case BITS: v = rc.read_uniform_bits(op.a); break;
        case SPLIT: v = rc.read_uniform_split(op.a); break;
        case SYMBOL: v = rc.decode_symbol(dec_ctx); break;
        case GOLOMB: v = rc.read_golomb(); break;
        }
        if (v != op.v) bad++;
    }
    CHECK(bad == 0);
}

void test_roundtrip_matrix() {
    for (int bps : {8, 16, 24, 32}) {
        for (int ch : {1, 2}) {
            std::vector<int32_t> s = make_signal(5000, ch, bps);
            for (int pred : {1, 2, 3}) {
                for (int ent : {1, 2, 3}) {
                    for (uint32_t block : {0u, 1500u}) {
                        OFR_EncodeParams p;
                        p.pred = pred;
                        p.ent = ent;
                        p.block_frames = block;
                        CHECK(decode_all(encode(s, ch, bps, p)) == s);
                    }
                }
            }
        }
    }
}

void test_edge_lengths() {
    for (uint64_t frames : {0ull, 1ull, 2ull, 3ull, 31ull, 33ull, 1024ull}) {
        for (int ch : {1, 2}) {
            std::vector<int32_t> s = make_signal(frames, ch, 16);
            for (int pred : {1, 2, 3}) {
                OFR_EncodeParams p;
                p.pred = pred;
                CHECK(decode_all(encode(s, ch, 16, p)) == s);
            }
        }
    }
    // full scale, including the most negative value
    std::vector<int32_t> s = {INT32_MIN, INT32_MAX, 0, -1, 1, INT32_MIN, INT32_MAX, INT32_MIN};
    OFR_EncodeParams p;
    CHECK(decode_all(encode(s, 2, 32, p)) == s);
}

void test_invalid_params() {
    std::vector<int32_t> s = make_signal(100, 2, 16);
    std::vector<uint8_t> file;
    OFR_EncodeParams p;
    CHECK(!ofr_encode(s.data(), 100, 3, 44100, 16, p, file));
    CHECK(!ofr_encode(s.data(), 100, 2, 44100, 12, p, file));
    p.pred = 4;
    CHECK(!ofr_encode(s.data(), 100, 2, 44100, 16, p, file));
    p = OFR_EncodeParams();
    p.stereo_order_idx = 8;   // order 256: past what the format's predictor can hold
    CHECK(!ofr_encode(s.data(), 100, 2, 44100, 16, p, file));
    p = OFR_EncodeParams();
    p.mono_order = 129;
    CHECK(!ofr_encode(s.data(), 100, 1, 44100, 16, p, file));
    p.mono_order = 128;
    CHECK(ofr_encode(s.data(), 100, 1, 44100, 16, p, file));
}

void test_blocks_may_switch_type() {
    const int ch = 2;
    std::vector<int32_t> s = make_signal(4000, ch, 16);
    const int types[][2] = {{1, 1}, {3, 3}, {2, 2}, {1, 3}, {3, 1}, {2, 3}, {1, 2}, {3, 2}};
    const uint32_t per_block = 500;

    std::vector<OFR_EncodedBlock> blocks;
    for (size_t b = 0; b < 8; b++) {
        OFR_EncodeParams p;
        p.pred = types[b][0];
        p.ent = types[b][1];
        blocks.push_back(ofr_encode_comp_block(s.data() + b * per_block * ch, per_block, ch, 16, 3, p));
    }
    std::vector<uint8_t> file;
    ofr_assemble_file(false, s.size(), 3, ch, 44100, blocks, file, {}, {});
    CHECK(decode_all(file) == s);
}

void test_search_and_threads() {
    std::vector<int32_t> s = make_signal(6000, 2, 16);
    OFR_EncodeParams plain;
    plain.block_frames = 1500;

    OFR_EncodeParams search = plain;
    search.search = true;
    std::vector<uint8_t> best = encode(s, 2, 16, search);
    CHECK(decode_all(best) == s);
    CHECK(best.size() <= encode(s, 2, 16, plain).size());

    search.threads = 4;
    CHECK(encode(s, 2, 16, search) == best);
}

void test_unsigned_samples() {
    std::vector<int32_t> s = make_signal(1000, 1, 8);
    OFR_EncodeParams p;
    p.unsigned_samples = true;
    std::vector<uint8_t> file = encode(s, 1, 8, p);
    std::string path = temp_path("unsigned.ofr");
    write_file(path, file);

    void* d = OptimFROG_createInstance();
    CHECK(OptimFROG_open(d, path.c_str()) == C_TRUE);
    OptimFROG_Info info{};
    OptimFROG_getInfo(d, &info);
    CHECK(strcmp(info.sampleType, "UINT8") == 0);
    std::vector<uint8_t> bytes(1000);
    CHECK(OptimFROG_read(d, bytes.data(), 1000) == 1000);
    bool same = true;
    for (size_t i = 0; i < bytes.size(); i++) same = same && bytes[i] == (uint8_t)(s[i] + 128);
    CHECK(same);
    OptimFROG_destroyInstance(d);
    remove(path.c_str());
}

void test_float() {
    const uint32_t specials[] = {0x00000000, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00001, 0x00000001, 0x80000001,
                                 0x007fffff, 0x3f800000, 0xbf800000, 0x7f7fffff, 0x00800000};
    for (int ch : {1, 2}) {
        for (uint32_t block : {0u, 700u}) {
            std::vector<float> s(4000);
            Rng rng(3);
            for (size_t i = 0; i < s.size(); i++) {
                uint32_t bits;
                float f = 0.5f * std::sin((float)i * 0.02f) + (float)(rng.next() % 1000) * 1e-6f;
                memcpy(&bits, &f, 4);
                if (i % 97 == 0) bits = specials[(i / 97) % 12];
                memcpy(&s[i], &bits, 4);
            }
            OFR_EncodeParams p;
            p.block_frames = block;
            std::vector<uint8_t> file;
            CHECK(ofr_encode_float(s.data(), s.size() / (size_t)ch, ch, 44100, p, file));

            ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
            CHECK(d.info().isFloat);
            std::vector<int32_t> out = d.readAll();
            CHECK(out.size() == s.size() && memcmp(out.data(), s.data(), s.size() * 4) == 0);
            CHECK(!d.hadRecoverableErrors());
        }
    }
}

// every subnormal magnitude class, alone in a block and next to ordinary samples
void test_subnormals() {
    for (int mix = 0; mix < 2; mix++) {
        std::vector<uint32_t> bits;
        for (int shift = 0; shift < 23; shift++) {
            for (uint32_t low : {0u, 1u, 0x155555u}) {
                uint32_t mant = (1u << shift) | (low & ((1u << shift) - 1));
                bits.push_back(mant);
                bits.push_back(0x80000000u | mant);
                if (mix) bits.push_back(0x3f123456u);
            }
        }
        bits.push_back(0x007fffff);
        std::vector<uint8_t> file;
        CHECK(ofr_encode_float((const float*)bits.data(), bits.size(), 1, 44100, OFR_EncodeParams(), file));
        std::vector<int32_t> out = decode_all(file);
        CHECK(out.size() == bits.size() && memcmp(out.data(), bits.data(), bits.size() * 4) == 0);
    }
}

// a single float block larger than what the decoder used to buffer
void test_large_float_block() {
    std::vector<float> s(1200000);
    for (size_t i = 0; i < s.size(); i++) s[i] = 0.25f * std::sin((float)i * 0.01f);
    std::vector<uint8_t> file;
    CHECK(ofr_encode_float(s.data(), s.size() / 2, 2, 44100, OFR_EncodeParams(), file));
    std::vector<int32_t> out = decode_all(file);
    CHECK(out.size() == s.size() && memcmp(out.data(), s.data(), s.size() * 4) == 0);
}

// a read request larger than what the decoder used to buffer
void test_large_read_request() {
    std::vector<int32_t> s = make_signal(600000, 2, 16);
    OFR_EncodeParams p;
    p.mono_order = 12;
    p.stereo_order_idx = 0;
    std::string path = temp_path("large.ofr");
    write_file(path, encode(s, 2, 16, p));

    void* d = OptimFROG_createInstance();
    CHECK(OptimFROG_open(d, path.c_str()) == C_TRUE);
    std::vector<int16_t> pcm(s.size());
    CHECK(OptimFROG_read(d, pcm.data(), 600000) == 600000);
    bool same = true;
    for (size_t i = 0; i < s.size(); i++) same = same && pcm[i] == s[i];
    CHECK(same);
    OptimFROG_destroyInstance(d);
    remove(path.c_str());
}

void append_id3v1(std::vector<uint8_t>& f) {
    uint8_t t[128] = {0};
    memcpy(t, "TAG", 3);
    memcpy(t + 3, "  Title in ID3  ", 16);
    memcpy(t + 33, "Artist", 6);
    memcpy(t + 93, "1999", 4);
    memcpy(t + 97, "comment", 7);
    t[126] = 7;     // track
    t[127] = 17;    // Rock
    f.insert(f.end(), t, t + 128);
}

void append_ape(std::vector<uint8_t>& f) {
    std::vector<uint8_t> items;
    auto item = [&items](const char* key, const char* value, uint32_t flags) {
        ofr_put32(items, (uint32_t)strlen(value));
        ofr_put32(items, flags);
        items.insert(items.end(), key, key + strlen(key) + 1);
        items.insert(items.end(), value, value + strlen(value));
    };
    item("title", "Title in APE", 0);
    item("Album", "An album", 0);
    item("Cover", "binary", 2);

    f.insert(f.end(), items.begin(), items.end());
    const char* id = "APETAGEX";
    f.insert(f.end(), id, id + 8);
    ofr_put32(f, 2000);
    ofr_put32(f, (uint32_t)items.size() + 32);
    ofr_put32(f, 3);
    ofr_put32(f, 0);
    f.insert(f.end(), 8, 0);
}

void test_c_api() {
    std::vector<int32_t> s = make_signal(20000, 2, 16);
    std::vector<uint8_t> head = {'h', 'e', 'a', 'd'}, tail = {'t', 'a', 'i', 'l', '!'};
    OFR_EncodeParams p;
    p.block_frames = 3000;
    std::vector<uint8_t> file = encode(s, 2, 16, p, head, tail);
    std::string path = temp_path("capi.ofr"), out = temp_path("capi.raw");
    write_file(path, file);

    CHECK(OptimFROG_getVersion() == 5100);

    void* d = OptimFROG_createInstance();
    CHECK(d != nullptr);
    CHECK(OptimFROG_close(d) == C_FALSE);
    CHECK(OptimFROG_open(d, "/nonexistent/file.ofr") == C_FALSE);
    CHECK(OptimFROG_open(d, path.c_str()) == C_TRUE);
    CHECK(OptimFROG_seekable(d) == C_TRUE);

    OptimFROG_Info info{};
    CHECK(OptimFROG_getInfo(d, &info) == C_TRUE);
    CHECK(info.channels == 2 && info.samplerate == 44100 && info.bitspersample == 16 && info.noPoints == 20000);
    CHECK(strcmp(info.sampleType, "SINT16") == 0 && strcmp(info.channelConfig, "STEREO_LR") == 0);
    CHECK(info.version == 5100 && info.length_ms == 453);
    CHECK(info.originalSize == 80000 + 4 && info.compressedSize == (sInt64_t)file.size());

    // both sizes can be queried: too small a buffer copies nothing and reports the size
    char small[8] = {0};
    CHECK(OptimFROG_readHead(d, small, 2) == 4 && small[0] == 0);
    CHECK(OptimFROG_readHead(d, small, sizeof(small)) == 4 && memcmp(small, "head", 4) == 0);
    CHECK(OptimFROG_readTail(d, small, sizeof(small)) == -1);

    std::vector<int16_t> pcm(s.size());
    CHECK(OptimFROG_read(d, pcm.data(), 20000) == 20000);
    CHECK(OptimFROG_getPos(d) == 20000);
    CHECK(OptimFROG_read(d, pcm.data(), 10) == 0);
    CHECK(OptimFROG_readTail(d, nullptr, 0) == 5);
    CHECK(OptimFROG_readTail(d, small, sizeof(small)) == 5 && memcmp(small, "tail!", 5) == 0);
    CHECK(OptimFROG_recoverableErrors(d) == C_FALSE);
    CHECK(OptimFROG_close(d) == C_TRUE);
    CHECK(OptimFROG_close(d) == C_FALSE);
    OptimFROG_destroyInstance(d);

    // max16bit keeps the top 16 bits of wider samples
    std::vector<int32_t> s24 = make_signal(100, 1, 24);
    std::string path24 = temp_path("capi24.ofr");
    write_file(path24, encode(s24, 1, 24, OFR_EncodeParams()));
    d = OptimFROG_createInstance();
    CHECK(OptimFROG_open(d, path24.c_str(), C_FALSE) == C_TRUE);
    std::vector<int16_t> pcm16(100);
    CHECK(OptimFROG_read(d, pcm16.data(), 100, C_TRUE) == 100);
    CHECK(pcm16[50] == (int16_t)(s24[50] >> 8));
    OptimFROG_destroyInstance(d);
    remove(path24.c_str());

    // decodeFile puts head, audio and tail back together
    int calls = 0;
    auto progress = [](void* param, Float64_t) { ++*(int*)param; };
    CHECK(OptimFROG_decodeFile(path.c_str(), out.c_str(), progress, &calls) == OptimFROG_NoError);
    CHECK(calls > 0);
    std::vector<uint8_t> decoded = read_file(out);
    CHECK(decoded.size() == head.size() + s.size() * 2 + tail.size());
    bool same = memcmp(decoded.data(), head.data(), head.size()) == 0;
    for (size_t i = 0; i < s.size(); i++) {
        int16_t v;
        memcpy(&v, &decoded[head.size() + i * 2], 2);
        same = same && v == s[i];
    }
    CHECK(same && memcmp(&decoded[decoded.size() - tail.size()], tail.data(), tail.size()) == 0);
    CHECK(OptimFROG_decodeFile("/nonexistent/file.ofr", out.c_str()) == OptimFROG_OpenError);
    CHECK(OptimFROG_decodeFile(path.c_str(), "/nonexistent/dir/out.raw") == OptimFROG_WriteError);

    OptimFROG_Info info2{};
    CHECK(OptimFROG_infoFile(path.c_str(), &info2) == OptimFROG_NoError);
    CHECK(info2.noPoints == 20000);
    CHECK(OptimFROG_infoFile("/nonexistent/file.ofr", &info2) == OptimFROG_OpenError);

    remove(path.c_str());
    remove(out.c_str());
}

void test_tags() {
    std::vector<uint8_t> file = encode(make_signal(100, 1, 16), 1, 16, OFR_EncodeParams());
    append_ape(file);
    append_id3v1(file);
    std::string path = temp_path("tags.ofr");
    write_file(path, file);

    OptimFROG_Info info{};
    OptimFROG_Tags tags{};
    CHECK(OptimFROG_infoFile(path.c_str(), &info, &tags) == OptimFROG_NoError);
    auto value = [&tags](const char* key) -> std::string {
        for (uInt32_t i = 0; i < tags.keyCount; i++) if (strcmp(tags.keys[i], key) == 0) return tags.values[i];
        return {};
    };
    CHECK(value("Title") == "Title in APE");   // the APE tag wins, under the first spelling of the key
    CHECK(value("Artist") == "Artist");
    CHECK(value("Album") == "An album");
    CHECK(value("Year") == "1999");
    CHECK(value("Comment") == "comment");
    CHECK(value("Track") == "7");
    CHECK(value("Genre") == "Rock");
    CHECK(value("Cover").empty());
    OptimFROG_freeTags(&tags);
    CHECK(tags.keyCount == 0);

    void* d = OptimFROG_createInstance();
    CHECK(OptimFROG_open(d, path.c_str(), C_FALSE) == C_TRUE);
    CHECK(OptimFROG_getTags(d, &tags) == C_FALSE && tags.keyCount == 0);
    OptimFROG_destroyInstance(d);

    ofrdecomp::Decoder decoder(path);
    CHECK(decoder.tags().size() == 7);
    remove(path.c_str());
}

// the reader must be released: batch use runs out of file descriptors otherwise
void test_no_descriptor_leak() {
    std::string path = temp_path("fd.ofr");
    write_file(path, encode(make_signal(50, 1, 16), 1, 16, OFR_EncodeParams()));
    bool ok = true;
    for (int i = 0; i < 20000 && ok; i++) {
        void* d = OptimFROG_createInstance();
        ok = OptimFROG_open(d, path.c_str()) == C_TRUE;
        OptimFROG_destroyInstance(d);
    }
    CHECK(ok);
    remove(path.c_str());
}

struct CountingReader {
    const std::vector<uint8_t>* data;
    size_t pos = 0;
    int closes = 0;
};

void test_reader_is_closed_once() {
    static ReadInterface ri = {
        [](void* i) -> condition_t { ((CountingReader*)i)->closes++; return C_TRUE; },
        [](void* i, void* dest, uInt32_t n) -> sInt32_t {
            auto* r = (CountingReader*)i;
            size_t got = std::min<size_t>(n, r->data->size() - r->pos);
            if (got) memcpy(dest, r->data->data() + r->pos, got);
            r->pos += got;
            return (sInt32_t)got;
        },
        [](void*) -> condition_t { return C_FALSE; },
        [](void*) -> condition_t { return C_FALSE; },
        [](void*) -> sInt64_t { return 0; },
        [](void*) -> sInt64_t { return 0; },
        [](void*, sInt64_t) -> condition_t { return C_FALSE; },
    };

    std::vector<uint8_t> file = encode(make_signal(500, 2, 16), 2, 16, OFR_EncodeParams());
    CountingReader good{&file};
    void* d = OptimFROG_createInstance();
    CHECK(OptimFROG_openExt(d, &ri, &good) == C_TRUE);
    CHECK(OptimFROG_seekable(d) == C_FALSE);
    CHECK(OptimFROG_seekPoint(d, 10) == C_FALSE);
    CHECK(OptimFROG_close(d) == C_TRUE);
    CHECK(good.closes == 1);
    OptimFROG_destroyInstance(d);
    CHECK(good.closes == 1);

    std::vector<uint8_t> junk(100, 0x55);
    CountingReader bad{&junk};
    d = OptimFROG_createInstance();
    CHECK(OptimFROG_openExt(d, &ri, &bad) == C_FALSE);
    OptimFROG_destroyInstance(d);
    CHECK(bad.closes == 1);
}

void test_seek() {
    for (int ch : {1, 2}) {
        std::vector<int32_t> s = make_signal(30000, ch, 16);
        OFR_EncodeParams p;
        p.block_frames = 2500;
        std::vector<uint8_t> file = encode(s, ch, 16, p);

        ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
        CHECK(d.seekable());
        Rng rng(11);
        std::vector<int32_t> got(200 * (size_t)ch);
        for (int i = 0; i < 60; i++) {
            int64_t frame = rng.next() % 29000;
            CHECK(d.seek(frame));
            CHECK(d.pos() == frame);
            CHECK(d.read(got, 200) == 200);
            CHECK(memcmp(got.data(), &s[(size_t)frame * (size_t)ch], got.size() * 4) == 0);
        }
        CHECK(!d.seek(30000));
        CHECK(!d.seek(-1));
        CHECK(d.seekTime(100));
        CHECK(d.pos() == 4410);
        CHECK(!d.hadRecoverableErrors());
    }

    // float blocks are followed by their DETA sub-block
    std::vector<float> f(20000);
    for (size_t i = 0; i < f.size(); i++) f[i] = std::sin((float)i * 0.05f);
    OFR_EncodeParams p;
    p.block_frames = 3000;
    std::vector<uint8_t> file;
    CHECK(ofr_encode_float(f.data(), f.size(), 1, 44100, p, file));
    ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
    std::vector<int32_t> got(100);
    for (int64_t frame : {15000, 200, 19000, 3100}) {
        CHECK(d.seek(frame));
        CHECK(d.read(got, 100) == 100);
        CHECK(memcmp(got.data(), &f[(size_t)frame], 400) == 0);
    }
}

void test_modern_api() {
    std::vector<int32_t> s = make_signal(3000, 2, 24);
    std::vector<uint8_t> head = {1, 2, 3}, tail = {4, 5};
    ofrdecomp::EncodeOptions options;
    options.predictor = 3;
    options.entropy = 2;
    options.blockFrames = 1000;
    std::vector<uint8_t> file = ofrdecomp::Encoder::encode(s, 2, 48000, 24, options, head, tail);

    ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
    ofrdecomp::Info info = d.info();
    CHECK(info.channels == 2 && info.samplerate == 48000 && info.bitspersample == 24 && info.noPoints == 3000);
    CHECK(info.sampleType == "SINT24" && info.channelConfig == "STEREO_LR" && !info.isFloat);
    CHECK(std::vector<uint8_t>(d.head().begin(), d.head().end()) == head);
    bool threw = false;
    try { d.tail(); } catch (const ofrdecomp::DecodeError&) { threw = true; }
    CHECK(threw);
    CHECK(d.readAll() == s);
    CHECK(std::vector<uint8_t>(d.tail().begin(), d.tail().end()) == tail);

    ofrdecomp::Decoder moved = std::move(d);
    CHECK(moved.pos() == 3000);

    threw = false;
    std::vector<int32_t> small(10);
    try { moved.read(small, 100); } catch (const ofrdecomp::DecodeError&) { threw = true; }
    CHECK(threw);

    threw = false;
    try { ofrdecomp::Decoder missing(std::string("/nonexistent/file.ofr")); } catch (const ofrdecomp::DecodeError& e) { threw = e.code() == OptimFROG_OpenError; }
    CHECK(threw);

    threw = false;
    try { ofrdecomp::Encoder::encode(s, 3, 44100, 16); } catch (const ofrdecomp::EncodeError&) { threw = true; }
    CHECK(threw);

    struct Sequential : ofrdecomp::Stream {
        const std::vector<uint8_t>& data;
        size_t pos = 0;
        explicit Sequential(const std::vector<uint8_t>& d) : data(d) {}
        size_t read(void* dest, size_t count) override {
            size_t n = std::min(count, data.size() - pos);
            memcpy(dest, data.data() + pos, n);
            pos += n;
            return n;
        }
    };
    ofrdecomp::Decoder streaming(std::make_unique<Sequential>(file));
    CHECK(!streaming.seekable());
    CHECK(!streaming.seek(10));
    CHECK(streaming.tags().empty());
    CHECK(streaming.readAll() == s);
}

// damage must never crash, hang or read out of bounds, and has to show up in the flag
void test_corruption() {
    Rng rng(99);
    const int types[][2] = {{1, 1}, {1, 2}, {1, 3}, {2, 1}, {3, 2}, {3, 3}};
    for (int ch : {1, 2}) {
        std::vector<int32_t> s = make_signal(3000, ch, 16);
        for (const auto& t : types) {
            OFR_EncodeParams p;
            p.pred = t[0];
            p.ent = t[1];
            p.block_frames = 1000;
            std::vector<uint8_t> file = encode(s, ch, 16, p);

            for (int round = 0; round < 60; round++) {
                std::vector<uint8_t> bad = file;
                size_t pos = 40 + rng.next() % (bad.size() - 40);
                bad[pos] ^= (uint8_t)(1u << (rng.next() % 8));
                ofrdecomp::Decoder d{std::span<const uint8_t>(bad)};
                std::vector<int32_t> out(s.size());
                size_t got = 0;
                try { got = d.read(out, s.size() / (size_t)ch); } catch (const ofrdecomp::DecodeError&) {}
                bool clean = got * (size_t)ch == s.size() && out == s;
                CHECK(clean || d.hadRecoverableErrors() || got * (size_t)ch < s.size());
            }

            for (size_t cut : {file.size() - 1, file.size() - 40, file.size() / 2, (size_t)60}) {
                std::vector<uint8_t> bad(file.begin(), file.begin() + (long)cut);
                try {
                    ofrdecomp::Decoder d{std::span<const uint8_t>(bad)};
                    std::vector<int32_t> out = d.readAll();
                    CHECK(out.size() < s.size() || d.hadRecoverableErrors() || cut == file.size() - 1);
                } catch (const ofrdecomp::DecodeError&) {}
            }
        }
    }

    // a damaged block doesn't take the following ones with it
    std::vector<int32_t> s = make_signal(4000, 2, 16);
    OFR_EncodeParams p;
    p.block_frames = 1000;
    std::vector<uint8_t> file = encode(s, 2, 16, p);
    size_t comp = 0;
    int seen = 0;
    for (size_t i = 0; i + 4 < file.size() && seen < 2; i++) {
        if (memcmp(&file[i], "COMP", 4) == 0) { comp = i; seen++; }
    }
    file[comp + 18] = 0x3f;   // the second block's type word: post type 63
    ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
    std::vector<int32_t> out = d.readAll();
    CHECK(d.hadRecoverableErrors());
    CHECK(out.size() == s.size());
    CHECK(memcmp(out.data(), s.data(), 2000 * 4) == 0);
    bool silent = true;
    for (size_t i = 2000; i < 4000; i++) silent = silent && out[i] == 0;
    CHECK(silent);
    CHECK(memcmp(out.data() + 4000, s.data() + 4000, (s.size() - 4000) * 4) == 0);
}

// losing the block structure ends the stream for good, however many reads follow
void test_read_after_fatal_error() {
    std::vector<int32_t> s = make_signal(2000, 1, 16);
    OFR_EncodeParams p;
    p.block_frames = 1000;
    std::vector<uint8_t> file = encode(s, 1, 16, p);
    size_t first = 0, second = 0;
    for (size_t i = 0; i + 4 < file.size(); i++) {
        if (memcmp(&file[i], "COMP", 4) == 0) {
            if (first == 0) first = i;
            else { second = i; break; }
        }
    }
    file[first + 18] = 0x3f;      // first block unusable...
    file.resize(second + 2);      // ...and the next one cut inside its magic

    ofrdecomp::Decoder d{std::span<const uint8_t>(file)};
    std::vector<int32_t> out(2000);
    CHECK(d.read(out, 1500) == 1000);
    bool threw = false;
    for (int i = 0; i < 3; i++) {
        try { d.read(out, 1000); } catch (const ofrdecomp::DecodeError&) { threw = true; }
    }
    CHECK(threw);
    CHECK(d.hadRecoverableErrors());
}

void test_rejected_headers() {
    auto rejected = [](std::vector<uint8_t> data) {
        try {
            ofrdecomp::Decoder d{std::span<const uint8_t>(data)};
        } catch (const ofrdecomp::DecodeError&) {
            return true;
        }
        return false;
    };
    std::vector<uint8_t> file = encode(make_signal(100, 1, 16), 1, 16, OFR_EncodeParams());
    CHECK(!rejected(file));
    CHECK(rejected({}));
    CHECK(rejected({'*', 'R', 'I', 'F', 'F', 0, 0, 0}));
    std::vector<uint8_t> bad = file;
    bad[14] = 11;                 // sample type
    CHECK(rejected(bad));
    bad = file;
    bad[15] = 2;                  // channel configuration
    CHECK(rejected(bad));
    bad = file;
    bad[4] = 12;                  // 4.5 alpha main block
    CHECK(rejected(bad));
    bad = file;
    bad[23] = 0xff; bad[24] = 0xff;   // needs a newer decoder
    CHECK(rejected(bad));

    // a self-extracting stub in front of the stream is fine
    std::vector<uint8_t> sfx(5000, 0x90);
    sfx.insert(sfx.end(), file.begin(), file.end());
    CHECK(!rejected(sfx));
}

void test_post_processor() {
    // DualStream: identity blocks clamp to the full sample range, not to the stored min/max
    OFR_PostProcessor pp;
    pp.m_channels = 1;
    pp.full_range_clamp = true;
    pp.min_val_L = -32768;
    pp.max_val_L = 32767;
    int32_t v[] = {40000, -40000, 123};
    pp.decode(v, 3);
    CHECK(v[0] == 32767 && v[1] == -32768 && v[2] == 123);

    // lossless identity blocks are left alone
    pp.full_range_clamp = false;
    pp.min_val_L = -10;
    pp.max_val_L = 10;
    int32_t w[] = {40000};
    pp.decode(w, 1);
    CHECK(w[0] == 40000);

    // and scaled ones are reconstructed
    pp.mult_L = 4;
    pp.offset_L = 2;
    pp.min_val_L = -100;
    pp.max_val_L = 100;
    int32_t x[] = {3, 1000, -1000};
    pp.decode(x, 3);
    CHECK(x[0] == 14 && x[1] == 100 && x[2] == -100);
}

void test_rounding_helpers() {
    CHECK(round_to_int32_cvtsd2si(0.5) == 0);
    CHECK(round_to_int32_cvtsd2si(1.5) == 2);
    CHECK(round_to_int32_cvtsd2si(2.5) == 2);
    CHECK(round_to_int32_cvtsd2si(-2.5) == -2);
    CHECK(round_to_int32_cvtsd2si(3e9) == INT32_MIN);
    CHECK(round_to_int32_cvtsd2si(-3e9) == INT32_MIN);
    CHECK(round_to_int32_cvtsd2si(std::nan("")) == INT32_MIN);
    CHECK(round_to_int32_cvtsd2si(INFINITY) == INT32_MIN);
    CHECK(trunc_to_int32_cvttsd2si(-1.9) == -1);
    CHECK(trunc_to_int32_cvttsd2si(1e10) == INT32_MIN);
    CHECK(trunc_to_int64_cvttsd2si(1e30) == INT64_MIN);
    CHECK(ofr_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
}

// the caller's floating point environment must not leak into the output
void test_fp_environment() {
    std::vector<int32_t> s = make_signal(3000, 2, 16);
    OFR_EncodeParams p;
    p.pred = 3;
    std::vector<uint8_t> expected = encode(s, 2, 16, p);

    std::fesetround(FE_UPWARD);
    std::vector<uint8_t> file = encode(s, 2, 16, p);
    std::vector<int32_t> out = decode_all(expected);
    std::fesetround(FE_TONEAREST);
    CHECK(file == expected);
    CHECK(out == s);
}

} // namespace

int main(int argc, char** argv) {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"range coder round trip", test_range_coder_roundtrip},
        {"round trip matrix", test_roundtrip_matrix},
        {"edge lengths", test_edge_lengths},
        {"invalid parameters", test_invalid_params},
        {"blocks may switch type", test_blocks_may_switch_type},
        {"search and threads", test_search_and_threads},
        {"unsigned samples", test_unsigned_samples},
        {"float", test_float},
        {"subnormals", test_subnormals},
        {"large float block", test_large_float_block},
        {"large read request", test_large_read_request},
        {"C API", test_c_api},
        {"tags", test_tags},
        {"no descriptor leak", test_no_descriptor_leak},
        {"reader closed once", test_reader_is_closed_once},
        {"seek", test_seek},
        {"modern API", test_modern_api},
        {"corruption", test_corruption},
        {"read after fatal error", test_read_after_fatal_error},
        {"rejected headers", test_rejected_headers},
        {"post processor", test_post_processor},
        {"rounding helpers", test_rounding_helpers},
        {"fp environment", test_fp_environment},
    };
    for (const Test& t : tests) {
        if (argc > 1 && strstr(t.name, argv[1]) == nullptr) continue;
        int before = failures;
        t.run();
        printf("%-28s %s\n", t.name, failures == before ? "ok" : "FAILED");
        fflush(stdout);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
