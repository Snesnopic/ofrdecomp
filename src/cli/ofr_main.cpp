// Unified CLI aiming for usage/flag parity with the original `ofr` tool, on top of our
// existing decode/encode engine. Supports the commands and options that matter for
// interoperability; deliberately skips rarely-used/deprecated original flags
// (--selfextract, --deleteafter, --timestamp, --incorrectheader, ...).
//
// Usage:
//   ofr --encode [options] src [--output dst] ...
//   ofr --decode [options] src [--output dst] ...
//   ofr --info src ...
//   ofr --help
#include "../../include/OptimFROG.h"
#include "../../include/optimfrog_encoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace {

// setenv is POSIX-only; MSVC has no equivalent besides _putenv_s.
static inline void ofr_setenv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// ---------------- minimal canonical PCM WAV read/write ----------------

struct WavInfo { uint32_t rate = 0; uint16_t channels = 0; uint16_t bits = 0; };

bool wav_read(const std::string& path, std::vector<uint8_t>& pcm, WavInfo& info) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char riff[4], wave[4];
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) { fclose(f); return false; }
    uint32_t riff_size; fread(&riff_size, 4, 1, f);
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) { fclose(f); return false; }

    bool have_fmt = false;
    while (true) {
        char id[4]; uint32_t sz;
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&sz, 4, 1, f) != 1) break;
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t fmt_tag, channels, block_align, bits; uint32_t rate, byte_rate;
            fread(&fmt_tag, 2, 1, f); fread(&channels, 2, 1, f); fread(&rate, 4, 1, f);
            fread(&byte_rate, 4, 1, f); fread(&block_align, 2, 1, f); fread(&bits, 2, 1, f);
            info.rate = rate; info.channels = channels; info.bits = bits;
            have_fmt = true;
            long extra = (long)sz - 16;
            if (extra > 0) fseek(f, extra, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0 && have_fmt) {
            pcm.resize(sz);
            fread(pcm.data(), 1, sz, f);
            break; // stop at the first data chunk (canonical layout)
        } else {
            fseek(f, (long)sz + (sz & 1), SEEK_CUR); // chunks are word-aligned
        }
    }
    fclose(f);
    return have_fmt && !pcm.empty();
}

bool wav_write(const std::string& path, const uint8_t* pcm, size_t pcm_bytes, uint32_t rate, uint16_t channels, uint16_t bits) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    uint32_t byte_rate = rate * block_align;
    uint32_t data_size = (uint32_t)pcm_bytes;
    uint32_t riff_size = 36 + data_size;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16; fwrite(&fmt_size, 4, 1, f);
    uint16_t fmt_tag = 1; fwrite(&fmt_tag, 2, 1, f);
    fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_size, 4, 1, f);
    fwrite(pcm, 1, pcm_bytes, f);
    fclose(f);
    return true;
}

// ---------------- preset -> encoder config ladder ----------------
// Not a bit-exact mirror of the reference's internal mode names (turbonew/highnew-light/...) --
// those are the reference ENCODER's internal knobs, irrelevant to interop since any valid,
// self-consistent stream our encoder writes is losslessly decodable by both decoders. This is a
// monotonically-more-effort ladder built from our own verified-safe encoder configurations.
struct PresetConfig { int pred; int ent; int mono_order; int od_idx; };

PresetConfig preset_config(const std::string& preset, int channels) {
    int level = 2; // reference's implicit default
    bool best_search = false;
    if (preset == "max") { level = 10; best_search = true; }
    else if (!preset.empty()) level = atoi(preset.c_str());

    static const int mono_orders[] = {24, 24, 64, 64, 96, 96, 96, 96, 96, 96, 96};
    static const int od_idxs[]     = { 0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1};
    if (level < 0) level = 0; if (level > 10) level = 10;

    PresetConfig c;
    c.mono_order = mono_orders[level];
    c.od_idx = od_idxs[level];
    c.pred = (level >= 4) ? 3 : 1;
    c.ent = (level >= 10) ? 3 : (level >= 2 ? 2 : 1);
    (void)best_search; (void)channels;
    return c;
}

void print_banner() {
    fprintf(stderr, "optimfrogcpp CLI (interop reimplementation) -- see doc/status.md for coverage\n");
}

void print_help() {
    print_banner();
    fprintf(stderr,
        "\nUsage:\n"
        "  ofr --encode [options] src [--output dst] ...\n"
        "  ofr --decode [options] src [--output dst] ...\n"
        "  ofr --info src ...\n"
        "  ofr --help\n"
        "\nCommands:\n"
        "  --encode              encode WAV/RAW file(s) to OFR file(s)\n"
        "  --decode              decode OFR file(s) to WAV/RAW file(s)\n"
        "  --info                print information about OFR file(s)\n"
        "  --help                this message\n"
        "\nOptions:\n"
        "  --preset level        0-10 or max (default: 2)\n"
        "  --raw                 treat input/output as headerless raw PCM (default: WAV)\n"
        "  --channelconfig {MONO|STEREO_LR}   required with --raw for --encode\n"
        "  --sampletype {UINT8|SINT8|UINT16|SINT16|UINT24|SINT24|UINT32|SINT32}\n"
        "                        required with --raw for --encode\n"
        "  --rate frequency      required with --raw for --encode\n"
        "  --output path         destination for the previous source file\n"
        "  --overwrite           overwrite an existing destination file\n"
        "  --silent              suppress the per-file progress line\n");
}

int sampletype_bits(const std::string& st, bool& is_signed) {
    is_signed = st[0] == 'S';
    if (st == "UINT8" || st == "SINT8") return 8;
    if (st == "UINT16" || st == "SINT16") return 16;
    if (st == "UINT24" || st == "SINT24") return 24;
    if (st == "UINT32" || st == "SINT32") return 32;
    return 0;
}

bool file_exists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

std::string default_output(const std::string& src, const char* new_ext) {
    size_t dot = src.find_last_of('.');
    std::string base = (dot == std::string::npos) ? src : src.substr(0, dot);
    return base + new_ext;
}

struct SrcDst { std::string src, dst; };

int do_encode(const std::vector<SrcDst>& files, const std::string& preset, bool raw,
              const std::string& channelconfig, const std::string& sampletype, uint32_t rate,
              bool overwrite, bool silent) {
    for (const auto& fd : files) {
        std::vector<uint8_t> pcm;
        uint32_t r = rate; int channels = 1, bps = 16;
        if (raw) {
            if (channelconfig.empty() || sampletype.empty() || rate == 0) {
                fprintf(stderr, "error: --raw --encode requires --channelconfig, --sampletype and --rate\n");
                return 1;
            }
            channels = (channelconfig == "STEREO_LR") ? 2 : 1;
            bool is_signed; bps = sampletype_bits(sampletype, is_signed);
            FILE* f = fopen(fd.src.c_str(), "rb");
            if (!f) { fprintf(stderr, "error: cannot open %s\n", fd.src.c_str()); return 1; }
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            pcm.resize(sz); fread(pcm.data(), 1, sz, f); fclose(f);
        } else {
            WavInfo wi;
            if (!wav_read(fd.src, pcm, wi)) { fprintf(stderr, "error: cannot read WAV %s\n", fd.src.c_str()); return 1; }
            r = wi.rate; channels = wi.channels; bps = wi.bits;
        }
        if (!overwrite && file_exists(fd.dst)) {
            fprintf(stderr, "error: %s exists (use --overwrite)\n", fd.dst.c_str());
            return 1;
        }
        int bytes = bps / 8;
        uint32_t nvals = (uint32_t)(pcm.size() / bytes);
        std::vector<int32_t> samples(nvals);
        for (uint32_t i = 0; i < nvals; i++) {
            const uint8_t* p = &pcm[(size_t)i * bytes];
            int32_t v = 0;
            for (int b = 0; b < bytes; b++) v |= (int32_t)p[b] << (8 * b);
            int shleft = 32 - bps; v = (v << shleft) >> shleft;
            samples[i] = v;
        }
        PresetConfig cfg = preset_config(preset, channels);
        ofr_setenv("OFR_PRED", std::to_string(cfg.pred).c_str());
        ofr_setenv("OFR_ENT", std::to_string(cfg.ent).c_str());
        ofr_setenv("OFR_ORDER", std::to_string(cfg.mono_order).c_str());
        ofr_setenv("OFR_ODIDX", std::to_string(cfg.od_idx).c_str());

        std::vector<uint8_t> ofr;
        bool ok = (channels == 2) ? ofr_encode_stereo(samples.data(), nvals / 2, r, bps, ofr)
                                   : ofr_encode_mono(samples.data(), nvals, r, bps, ofr);
        if (!ok) { fprintf(stderr, "error: encode failed for %s\n", fd.src.c_str()); return 1; }
        FILE* out = fopen(fd.dst.c_str(), "wb");
        if (!out) { fprintf(stderr, "error: cannot write %s\n", fd.dst.c_str()); return 1; }
        fwrite(ofr.data(), 1, ofr.size(), out); fclose(out);
        if (!silent) fprintf(stderr, "%s -> %s (%zu bytes)\n", fd.src.c_str(), fd.dst.c_str(), ofr.size());
    }
    return 0;
}

int do_decode(const std::vector<SrcDst>& files, bool raw, bool overwrite, bool silent) {
    for (const auto& fd : files) {
        if (!overwrite && file_exists(fd.dst)) {
            fprintf(stderr, "error: %s exists (use --overwrite)\n", fd.dst.c_str());
            return 1;
        }
        void* inst = OptimFROG_createInstance();
        if (!inst || OptimFROG_open(inst, const_cast<char*>(fd.src.c_str()), 0) != OptimFROG_NoError) {
            fprintf(stderr, "error: cannot open %s\n", fd.src.c_str());
            return 1;
        }
        OptimFROG_Info info; memset(&info, 0, sizeof(info));
        OptimFROG_getInfo(inst, &info);
        int bpv = info.bitspersample / 8;
        std::vector<uint8_t> pcm((size_t)info.noPoints * info.channels * bpv);
        const uint32_t CHUNK = 65536;
        std::vector<uint8_t> buf((size_t)CHUNK * info.channels * bpv);
        size_t done = 0;
        while (done < (size_t)info.noPoints) {
            uint32_t want = (uint32_t)std::min<long long>(CHUNK, info.noPoints - done);
            int got = OptimFROG_read(inst, buf.data(), want, 0);
            if (got <= 0) break;
            memcpy(&pcm[done * info.channels * bpv], buf.data(), (size_t)got * info.channels * bpv);
            done += got;
        }
        OptimFROG_close(inst); OptimFROG_destroyInstance(inst);

        if (raw) {
            FILE* out = fopen(fd.dst.c_str(), "wb");
            if (!out) { fprintf(stderr, "error: cannot write %s\n", fd.dst.c_str()); return 1; }
            fwrite(pcm.data(), 1, pcm.size(), out); fclose(out);
        } else {
            wav_write(fd.dst, pcm.data(), pcm.size(), info.samplerate, (uint16_t)info.channels, (uint16_t)info.bitspersample);
        }
        if (!silent) fprintf(stderr, "%s -> %s (%lld frames)\n", fd.src.c_str(), fd.dst.c_str(), (long long)info.noPoints);
    }
    return 0;
}

int do_info(const std::vector<std::string>& srcs) {
    printf("%-40s %6s %3s %5s %10s %8s\n", "Filename", "Rate", "Ch", "Bits", "Duration", "Points");
    for (const auto& src : srcs) {
        void* inst = OptimFROG_createInstance();
        if (!inst || OptimFROG_open(inst, const_cast<char*>(src.c_str()), 0) != OptimFROG_NoError) {
            fprintf(stderr, "error: cannot open %s\n", src.c_str());
            continue;
        }
        OptimFROG_Info info; memset(&info, 0, sizeof(info));
        OptimFROG_getInfo(inst, &info);
        printf("%-40s %6u %3u %5u %9lldms %8lld\n", src.c_str(), info.samplerate, info.channels,
               info.bitspersample, (long long)info.length_ms, (long long)info.noPoints);
        OptimFROG_close(inst); OptimFROG_destroyInstance(inst);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }

    enum { NONE, ENCODE, DECODE, INFO, HELP } command = NONE;
    std::string preset, channelconfig, sampletype;
    uint32_t rate = 0;
    bool raw = false, overwrite = false, silent = false;
    std::vector<SrcDst> files;
    std::vector<std::string> info_srcs;
    std::string pending_src;

    auto flush_pending = [&](const std::string& explicit_dst) {
        if (pending_src.empty()) return;
        if (command == INFO) { info_srcs.push_back(pending_src); }
        else {
            std::string dst = explicit_dst.empty()
                ? default_output(pending_src, command == ENCODE ? ".ofr" : (raw ? ".raw" : ".wav"))
                : explicit_dst;
            files.push_back({pending_src, dst});
        }
        pending_src.clear();
    };

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--encode") { command = ENCODE; }
        else if (a == "--decode") { command = DECODE; }
        else if (a == "--info") { command = INFO; }
        else if (a == "--help") { command = HELP; }
        else if (a == "--raw") { raw = true; }
        else if (a == "--overwrite") { overwrite = true; }
        else if (a == "--silent") { silent = true; }
        else if (a == "--preset" && i + 1 < argc) { preset = argv[++i]; }
        else if (a == "--channelconfig" && i + 1 < argc) { channelconfig = argv[++i]; }
        else if (a == "--sampletype" && i + 1 < argc) { sampletype = argv[++i]; }
        else if (a == "--rate" && i + 1 < argc) { rate = (uint32_t)atoi(argv[++i]); }
        else if (a == "--output" && i + 1 < argc) { flush_pending(argv[++i]); }
        else if (a.size() > 2 && a[0] == '-' && a[1] == '-') { fprintf(stderr, "warning: ignoring unrecognized option %s\n", a.c_str()); }
        else { flush_pending(""); pending_src = a; }
    }
    flush_pending("");

    if (command == HELP || command == NONE) { print_help(); return command == HELP ? 0 : 1; }
    if (command == ENCODE) return do_encode(files, preset, raw, channelconfig, sampletype, rate, overwrite, silent);
    if (command == DECODE) return do_decode(files, raw, overwrite, silent);
    if (command == INFO) return do_info(info_srcs);
    return 1;
}
