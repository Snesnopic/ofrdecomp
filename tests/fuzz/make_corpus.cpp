// writes a seed corpus covering every predictor and entropy coder, for tests/fuzz/fuzz_decoder.cpp.
#include "../../include/optimfrog_encoder.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    int index = 0;
    auto save = [&](const std::vector<uint8_t>& file) {
        std::string path = dir + "/seed" + std::to_string(index++);
        if (FILE* f = fopen(path.c_str(), "wb")) {
            fwrite(file.data(), 1, file.size(), f);
            fclose(f);
        }
    };

    for (int ch : {1, 2}) {
        std::vector<int32_t> s(600 * (size_t)ch);
        std::vector<float> fl(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            s[i] = (int32_t)(9000.0 * std::sin((double)i * 0.05)) + (int32_t)(i * 2654435761u >> 28);
            fl[i] = (float)s[i] / 32768.0f;
        }
        for (int pred : {1, 2, 3}) {
            for (int ent : {1, 2, 3}) {
                OFR_EncodeParams p;
                p.pred = pred;
                p.ent = ent;
                p.block_frames = 256;
                std::vector<uint8_t> file;
                if (ofr_encode(s.data(), 600, ch, 44100, 16, p, file, {1, 2, 3}, {4, 5})) save(file);
            }
        }
        OFR_EncodeParams p;
        p.block_frames = 256;
        std::vector<uint8_t> file;
        if (ofr_encode_float(fl.data(), 600, ch, 44100, p, file)) save(file);
    }
    return 0;
}
