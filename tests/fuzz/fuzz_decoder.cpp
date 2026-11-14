// libFuzzer harness: anything the decoder can be fed must neither crash nor hang.
// build with -DOFRDECOMP_FUZZ=ON and clang, run with a corpus of small encoded files.
#include "../../include/ofrdecomp.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        ofrdecomp::Decoder d{std::span<const uint8_t>(data, size)};
        ofrdecomp::Info info = d.info();
        d.tags();

        // a damaged block decodes to its end like in the reference, so the work grows with the
        // length the file claims: keep it bounded, seeking only in short files
        std::vector<int32_t> buffer(4096 * 2);
        for (int i = 0; i < 16; i++) {
            if (d.read(buffer, 4096) == 0) break;
        }
        if (info.noPoints > 0 && info.noPoints <= 65536) {
            d.seek(info.noPoints / 2);
            d.read(buffer, 256);
            d.seek(0);
            d.read(buffer, 256);
        }
    } catch (const ofrdecomp::DecodeError&) {
    }
    return 0;
}
