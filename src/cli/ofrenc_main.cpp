// ofrenc: test-harness encoder with positional arguments, tuned through OFR_* environment
// variables. the interactive tool is `ofr`; the conformance suite drives this one.
#include "../../include/optimfrog_encoder.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

static OFR_EncodeParams params_from_env() {
    OFR_EncodeParams p;
    auto env_int = [](const char* name, auto& field) {
        if (const char* v = getenv(name)) field = (std::remove_reference_t<decltype(field)>)atoi(v);
    };
    env_int("OFR_PRED", p.pred);
    env_int("OFR_ENT", p.ent);
    env_int("OFR_ORDER", p.mono_order);
    env_int("OFR_INTERVAL", p.mono_interval);
    env_int("OFR_ODIDX", p.stereo_order_idx);
    env_int("OFR_IVIDX", p.stereo_interval_idx);
    env_int("OFR_WPRED", p.pred_weight);
    env_int("OFR_WENT", p.ent_weight);
    env_int("OFR_BLOCK", p.block_frames);
    env_int("OFR_THREADS", p.threads);
    p.search = getenv("OFR_BEST") != nullptr;
    return p;
}

static bool read_file(const char* path, std::vector<uint8_t>& data) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    data.clear();
    std::vector<uint8_t> chunk(1u << 20);
    size_t got;
    while ((got = fread(chunk.data(), 1, chunk.size(), f)) > 0) data.insert(data.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t)got);
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

static int run(int argc, char** argv) {
    // --float: encode a raw float32 PCM file (IEEE Float / "OFRX") instead of integer PCM.
    bool want_float = false;
    std::vector<char*> pos;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--float") want_float = true;
        else pos.push_back(argv[i]);
    }
    if (pos.size() < 3) {
        fprintf(stderr, "usage: %s in.raw out.ofr samplerate [channels=1] [bps=16] [--float]\n  (set OFR_BEST=1 to search configs; --float ignores bps, always 32-bit float)\n", argv[0]);
        return 2;
    }
    int ch = (pos.size() >= 4) ? atoi(pos[3]) : 1;
    uint32_t rate = (uint32_t)atoi(pos[2]);
    int bps = want_float ? 32 : (pos.size() >= 5) ? atoi(pos[4]) : 16;
    OFR_EncodeParams params = params_from_env();

    std::vector<uint8_t> raw;
    if (!read_file(pos[0], raw)) { perror("in"); return 1; }

    std::vector<uint8_t> out;
    bool ok;
    if (want_float) {
        std::vector<float> s(raw.size() / 4);
        if (!s.empty()) memcpy(s.data(), raw.data(), s.size() * 4);
        ok = ofr_encode_float(s.data(), s.size() / (size_t)ch, ch, rate, params, out);
    } else {
        int bytes = bps / 8;
        if (bytes < 1 || bytes > 4) { fprintf(stderr, "unsupported bit depth\n"); return 2; }
        std::vector<int32_t> s(raw.size() / (size_t)bytes);
        for (size_t i = 0; i < s.size(); i++) {
            uint32_t v = 0;
            for (int b = 0; b < bytes; b++) v |= (uint32_t)raw[i * (size_t)bytes + (size_t)b] << (8 * b);
            int shleft = 32 - bps;
            s[i] = (int32_t)(v << shleft) >> shleft;
        }
        ok = ofr_encode(s.data(), s.size() / (size_t)ch, ch, rate, bps, params, out);
    }
    if (!ok) { fprintf(stderr, "invalid encoder parameters\n"); return 1; }

    FILE* g = fopen(pos[1], "wb");
    if (!g) { perror("out"); return 1; }
    fwrite(out.data(), 1, out.size(), g);
    fclose(g);
    fprintf(stderr, "wrote %zu bytes (ch=%d bps=%d)\n", out.size(), ch, bps);
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    }
}
