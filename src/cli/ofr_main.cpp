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
#include "../../include/ofr_common.h"
#include "../../include/optimfrog_encoder.h"
#include "../../include/md5.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>

namespace {

bool read_whole_file(const std::string& path, std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    data.clear();
    std::vector<uint8_t> chunk(1u << 20);
    size_t got;
    while ((got = fread(chunk.data(), 1, chunk.size(), f)) > 0) data.insert(data.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t)got);
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ---------------- WAV read/write ----------------

constexpr uint16_t WAV_FORMAT_PCM = 1, WAV_FORMAT_FLOAT = 3, WAV_FORMAT_EXTENSIBLE = 0xfffe;

struct WavInfo {
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t format = 0;
    size_t data_pos = 0;    // the PCM payload; what's around it is kept verbatim as HEAD and TAIL
    size_t data_size = 0;
};

bool wav_parse(const std::vector<uint8_t>& file, WavInfo& info) {
    if (file.size() < 12 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    size_t pos = 12;
    while (file.size() - pos >= 8) {
        const uint8_t* chunk = file.data() + pos;
        size_t size = le32(chunk + 4), body = pos + 8;
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16 && file.size() - body >= 16) {
            const uint8_t* f = file.data() + body;
            info.format = le16(f);
            info.channels = le16(f + 2);
            info.rate = le32(f + 4);
            info.bits = le16(f + 14);
            if (info.format == WAV_FORMAT_EXTENSIBLE && size >= 26 && file.size() - body >= 26) info.format = le16(f + 24);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0 && have_fmt) {
            info.data_pos = body;
            info.data_size = std::min(size, file.size() - body);   // truncated or streamed files
            return true;
        }
        if (file.size() - body < size) return false;
        pos = body + size + (size & 1);   // chunks are word-aligned
        if (pos > file.size()) return false;
    }
    return false;
}

std::vector<uint8_t> wav_header(uint64_t pcm_bytes, uint32_t rate, uint16_t channels, uint16_t bits, bool is_float) {
    uint32_t data_size = (uint32_t)std::min<uint64_t>(pcm_bytes, 0xffffffffu - 36);
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    std::vector<uint8_t> h;
    auto put16 = [&h](uint16_t v) { h.push_back((uint8_t)v); h.push_back((uint8_t)(v >> 8)); };
    auto put_str = [&h](const char* s) { h.insert(h.end(), s, s + 4); };

    put_str("RIFF"); ofr_put32(h, 36 + data_size); put_str("WAVE");
    put_str("fmt "); ofr_put32(h, 16);
    put16(is_float ? WAV_FORMAT_FLOAT : WAV_FORMAT_PCM);
    put16(channels); ofr_put32(h, rate); ofr_put32(h, rate * block_align);
    put16(block_align); put16(bits);
    put_str("data"); ofr_put32(h, data_size);
    return h;
}

// ---------------- raw container block scanner (--verify / --check) ----------------
// Walks the OFR/HEAD/COMP*/TAIL/MD5 block structure directly on the raw file bytes,
// independent of the decode engine -- --verify only needs each COMP block's declared
// CRC32 to match, and --check only needs the trailing MD5 block's stored hash. Neither
// requires actually decoding samples.

struct ScanResult { bool ok = false; bool has_md5 = false; uint8_t md5[16] = {0}; std::string error; };

ScanResult scan_ofr_file(const std::string& path) {
    ScanResult r;
    std::vector<uint8_t> buf;
    if (!read_whole_file(path, buf)) { r.error = "cannot read file"; return r; }

    size_t sz = buf.size();
    auto has = [&](size_t pos, size_t n) { return pos <= sz && sz - pos >= n; };
    auto is_tag = [&](size_t pos, const char* tag) { return has(pos, 8) && memcmp(&buf[pos], tag, 4) == 0; };

    if (!is_tag(0, "OFR ") && !is_tag(0, "OFRX")) { r.error = "not an OFR file"; return r; }
    size_t pos = 8 + (size_t)le32(&buf[4]);
    if (!is_tag(pos, "HEAD")) { r.error = "malformed file (no HEAD block)"; return r; }
    pos += 8 + (size_t)le32(&buf[pos + 4]);

    while (is_tag(pos, "COMP") || is_tag(pos, "DETA")) {
        uint32_t size = le32(&buf[pos + 4]);
        if (size < 4 || !has(pos + 8, size)) { r.error = "truncated block"; return r; }
        if (ofr_crc32(&buf[pos + 12], size - 4) != le32(&buf[pos + 8])) { r.error = "CRC32 mismatch in a block"; return r; }
        pos += 8 + (size_t)size;
    }
    if (is_tag(pos, "TAIL")) pos += 8 + (size_t)le32(&buf[pos + 4]);

    // optional trailing MD5/RECV blocks
    while (has(pos, 8)) {
        if (is_tag(pos, "MD5 ")) {
            uint32_t msz = le32(&buf[pos + 4]);
            if (msz == 16 && has(pos + 8, 16)) { r.has_md5 = true; memcpy(r.md5, &buf[pos + 8], 16); }
            pos += 8 + (size_t)msz;
        } else if (is_tag(pos, "RECV")) {
            pos += 8 + (size_t)le32(&buf[pos + 4]);
        } else {
            break; // tags and the like; nothing more we need here
        }
    }
    r.ok = true;
    return r;
}

// an OptimFROG_* decoder instance
class Instance {
public:
    Instance() : inst_(OptimFROG_createInstance()) {}
    ~Instance() { OptimFROG_destroyInstance(inst_); }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    operator void*() const { return inst_; }
private:
    void* inst_;
};

int do_verify(const std::vector<std::string>& srcs, bool verbose) {
    int failures = 0;
    for (const auto& src : srcs) {
        ScanResult r = scan_ofr_file(src);
        if (r.ok) { if (verbose) fprintf(stderr, "%s: OK\n", src.c_str()); }
        else { fprintf(stderr, "%s: FAILED (%s)\n", src.c_str(), r.error.c_str()); failures++; }
    }
    return failures ? 1 : 0;
}

int do_check(const std::vector<std::string>& srcs, bool verbose) {
    int failures = 0;
    for (const auto& src : srcs) {
        ScanResult r = scan_ofr_file(src);
        if (!r.ok) { fprintf(stderr, "%s: FAILED (%s)\n", src.c_str(), r.error.c_str()); failures++; continue; }
        if (!r.has_md5) { fprintf(stderr, "%s: no MD5 signature stored (encode with --md5)\n", src.c_str()); failures++; continue; }

        Instance inst;
        if (!inst || !OptimFROG_open(inst, src.c_str())) {
            fprintf(stderr, "%s: FAILED (cannot open for decode)\n", src.c_str()); failures++; continue;
        }
        OptimFROG_Info info{};
        OptimFROG_getInfo(inst, &info);
        int bpv = info.bitspersample / 8;
        MD5 md5;
        const uint32_t CHUNK = 65536;
        std::vector<uint8_t> chunkbuf((size_t)CHUNK * info.channels * bpv);
        long long done = 0;
        while (done < info.noPoints) {
            uint32_t want = (uint32_t)std::min<long long>(CHUNK, info.noPoints - done);
            int got = OptimFROG_read(inst, chunkbuf.data(), want, 0);
            if (got <= 0) break;
            md5.update(chunkbuf.data(), (size_t)got * info.channels * bpv);
            done += got;
        }

        uint8_t computed[16]; md5.finish(computed);
        bool match = memcmp(computed, r.md5, 16) == 0;
        if (match) { if (verbose) fprintf(stderr, "%s: OK\n", src.c_str()); }
        else { fprintf(stderr, "%s: FAILED (MD5 mismatch)\n", src.c_str()); failures++; }
    }
    return failures ? 1 : 0;
}

// ---------------- preset -> encoder config ladder ----------------
// Not a bit-exact mirror of the reference's internal mode names (turbonew/highnew-light/...) --
// those are the reference ENCODER's internal knobs, irrelevant to interop since any valid,
// self-consistent stream our encoder writes is losslessly decodable by both decoders. This is a
// monotonically-more-effort ladder built from our own verified-safe encoder configurations.
OFR_EncodeParams preset_params(const std::string& preset) {
    int level = 2; // reference's implicit default
    bool best_search = false;
    if (preset == "max") { level = 10; best_search = true; }
    else if (!preset.empty()) level = atoi(preset.c_str());

    static const int mono_orders[] = {24, 24, 64, 64, 96, 96, 96, 96, 96, 96, 96};
    static const int od_idxs[]     = { 0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1};
    level = std::clamp(level, 0, 10);

    OFR_EncodeParams p;
    p.mono_order = mono_orders[level];
    p.stereo_order_idx = od_idxs[level];
    p.pred = (level >= 4) ? 3 : 1;
    p.ent = (level >= 10) ? 3 : (level >= 2 ? 2 : 1);
    p.search = best_search;
    p.block_frames = 1u << 20;
    return p;
}

void print_banner() {
    fprintf(stderr, "ofrdecomp CLI (interop reimplementation) -- see doc/status.md for coverage\n");
}

void print_help() {
    print_banner();
    fprintf(stderr,
        "\nUsage:\n"
        "  ofr --encode [options] src [--output dst] ...\n"
        "  ofr --decode [options] src [--output dst] ...\n"
        "  ofr --info src ...\n"
        "  ofr --verify src ...\n"
        "  ofr --check src ...\n"
        "  ofr --help\n"
        "\nCommands:\n"
        "  --encode              encode WAV/RAW file(s) to OFR file(s)\n"
        "  --decode              decode OFR file(s) to WAV/RAW file(s)\n"
        "  --info                print information about OFR file(s)\n"
        "  --verify              quick integrity check (COMP block CRC32) of OFR file(s)\n"
        "  --check               verify the stored MD5 of raw PCM input data (needs --md5 at encode time)\n"
        "  --help                this message\n"
        "\nOptions:\n"
        "  --preset level        0-10 or max (default: 2)\n"
        "  --threads count       blocks encoded in parallel (default: 1, 0 for one per core)\n"
        "  --raw                 treat input/output as headerless raw PCM (default: WAV)\n"
        "  --channelconfig {MONO|STEREO_LR}   required with --raw for --encode\n"
        "  --sampletype {UINT8|SINT8|UINT16|SINT16|UINT24|SINT24|UINT32|SINT32|FLOAT32_1}\n"
        "                        required with --raw for --encode\n"
        "  --rate frequency      required with --raw for --encode\n"
        "  --headersize size     (--raw --encode) save the first size bytes as an opaque header\n"
        "  --tailsize size       (--raw --encode) save the last size bytes as an opaque tail\n"
        "  --writefreshheader    (--decode) write a standard WAV header instead of the stored one\n"
        "  --output path         destination for the previous source file\n"
        "  --overwrite           overwrite an existing destination file\n"
        "  --silent              suppress the per-file progress line\n"
        "  --verbose             print extra per-file details\n"
        "  --time                print elapsed time per operation\n"
        "  --md5                 store an MD5 signature of the raw PCM input data (--encode)\n");
}

int sampletype_bits(const std::string& st) {
    if (st == "UINT8" || st == "SINT8") return 8;
    if (st == "UINT16" || st == "SINT16") return 16;
    if (st == "UINT24" || st == "SINT24") return 24;
    if (st == "UINT32" || st == "SINT32" || st == "FLOAT32_1") return 32;
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

struct EncodeOptions {
    std::string preset, channelconfig, sampletype;
    uint32_t rate = 0, headersize = 0, tailsize = 0, threads = 1;
    bool raw = false, overwrite = false, silent = false, verbose = false, want_md5 = false, show_time = false;
};

int do_encode(const std::vector<SrcDst>& files, const EncodeOptions& opt) {
    for (const auto& fd : files) {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> input, head, tail;
        if (!read_whole_file(fd.src, input)) { fprintf(stderr, "error: cannot read %s\n", fd.src.c_str()); return 1; }

        const uint8_t* pcm = nullptr;
        size_t pcm_size = 0;
        uint32_t rate = opt.rate;
        int channels = 1, bps = 16;
        bool is_unsigned = false, is_float = false;
        if (opt.raw) {
            bps = sampletype_bits(opt.sampletype);
            if (opt.channelconfig.empty() || bps == 0 || rate == 0) {
                fprintf(stderr, "error: --raw --encode requires --channelconfig, --sampletype and --rate\n");
                return 1;
            }
            channels = (opt.channelconfig == "STEREO_LR") ? 2 : 1;
            is_unsigned = opt.sampletype[0] == 'U';
            is_float = opt.sampletype[0] == 'F';
            if ((size_t)opt.headersize + opt.tailsize > input.size()) {
                fprintf(stderr, "error: --headersize + --tailsize exceeds file size for %s\n", fd.src.c_str());
                return 1;
            }
            head.assign(input.begin(), input.begin() + opt.headersize);
            tail.assign(input.end() - opt.tailsize, input.end());
            pcm = input.data() + opt.headersize;
            pcm_size = input.size() - opt.headersize - opt.tailsize;
        } else {
            if (opt.headersize > 0 || opt.tailsize > 0) {
                fprintf(stderr, "error: --headersize/--tailsize require --raw\n");
                return 1;
            }
            WavInfo wi;
            if (!wav_parse(input, wi)) { fprintf(stderr, "error: cannot read WAV %s\n", fd.src.c_str()); return 1; }
            is_float = wi.format == WAV_FORMAT_FLOAT;
            if ((wi.format != WAV_FORMAT_PCM && !is_float) || (is_float && wi.bits != 32) ||
                (wi.channels != 1 && wi.channels != 2) || sampletype_bits("SINT" + std::to_string(wi.bits)) == 0) {
                fprintf(stderr, "error: unsupported WAV format in %s\n", fd.src.c_str());
                return 1;
            }
            rate = wi.rate; channels = wi.channels; bps = wi.bits;
            is_unsigned = bps == 8;   // the WAV convention
            head.assign(input.begin(), input.begin() + (long)wi.data_pos);
            tail.assign(input.begin() + (long)(wi.data_pos + wi.data_size), input.end());
            pcm = input.data() + wi.data_pos;
            pcm_size = wi.data_size;
        }
        if (!opt.overwrite && file_exists(fd.dst)) {
            fprintf(stderr, "error: %s exists (use --overwrite)\n", fd.dst.c_str());
            return 1;
        }

        OFR_EncodeParams params = preset_params(opt.preset);
        params.threads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
        params.unsigned_samples = is_unsigned;

        const int bytes = bps / 8;
        const size_t nvals = pcm_size / (size_t)bytes;
        const uint64_t frames = nvals / (size_t)channels;
        std::vector<uint8_t> ofr;
        bool ok;
        if (is_float) {
            std::vector<float> samples(nvals);
            if (nvals) memcpy(samples.data(), pcm, nvals * 4);
            ok = ofr_encode_float(samples.data(), frames, channels, rate, params, ofr, head, tail);
        } else {
            const uint32_t bias = is_unsigned ? 1u << (bps - 1) : 0;
            const int shleft = 32 - bps;
            std::vector<int32_t> samples(nvals);
            for (size_t i = 0; i < nvals; i++) {
                const uint8_t* p = pcm + i * (size_t)bytes;
                uint32_t v = 0;
                for (int b = 0; b < bytes; b++) v |= (uint32_t)p[b] << (8 * b);
                samples[i] = (int32_t)((v - bias) << shleft) >> shleft;
            }
            ok = ofr_encode(samples.data(), frames, channels, rate, bps, params, ofr, head, tail);
        }
        if (!ok) { fprintf(stderr, "error: encode failed for %s\n", fd.src.c_str()); return 1; }

        if (opt.want_md5) {
            uint8_t hash[16]; md5_compute(pcm, pcm_size, hash);
            const uint8_t blockId[4] = {'M','D','5',' '};
            ofr.insert(ofr.end(), blockId, blockId + 4);
            ofr_put32(ofr, 16);
            ofr.insert(ofr.end(), hash, hash + 16);
        }

        FILE* out = fopen(fd.dst.c_str(), "wb");
        if (!out) { fprintf(stderr, "error: cannot write %s\n", fd.dst.c_str()); return 1; }
        size_t written = fwrite(ofr.data(), 1, ofr.size(), out);
        if (fclose(out) != 0 || written != ofr.size()) { fprintf(stderr, "error: cannot write %s\n", fd.dst.c_str()); return 1; }
        if (opt.verbose) fprintf(stderr, "%s: pred=%d ent=%d order=%d od_idx=%d search=%d\n", fd.src.c_str(), params.pred, params.ent, params.mono_order, params.stereo_order_idx, params.search ? 1 : 0);
        if (!opt.silent) fprintf(stderr, "%s -> %s (%zu bytes)\n", fd.src.c_str(), fd.dst.c_str(), ofr.size());
        if (opt.show_time) {
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "%s: %.3fs\n", fd.src.c_str(), secs);
        }
    }
    return 0;
}

int do_decode(const std::vector<SrcDst>& files, bool raw, bool fresh_header, bool overwrite, bool silent, bool verbose, bool show_time) {
    for (const auto& fd : files) {
        auto t0 = std::chrono::steady_clock::now();
        if (!overwrite && file_exists(fd.dst)) {
            fprintf(stderr, "error: %s exists (use --overwrite)\n", fd.dst.c_str());
            return 1;
        }
        Instance inst;
        if (!inst || !OptimFROG_open(inst, fd.src.c_str())) {
            fprintf(stderr, "error: cannot open %s\n", fd.src.c_str());
            return 1;
        }
        OptimFROG_Info info{};
        OptimFROG_getInfo(inst, &info);
        const size_t frame_bytes = (size_t)(info.bitspersample / 8) * info.channels;

        FILE* out = fopen(fd.dst.c_str(), "wb");
        if (!out) { fprintf(stderr, "error: cannot write %s\n", fd.dst.c_str()); return 1; }
        auto put = [out](const std::vector<uint8_t>& bytes, size_t n) { return n == 0 || fwrite(bytes.data(), 1, n, out) == n; };
        bool ok = true;

        // the stored head and tail put the original file back together. a file without them
        // has no container to restore, so a WAV destination gets a standard header.
        std::vector<uint8_t> head((size_t)OptimFROG_readHead(inst, nullptr, 0));
        OptimFROG_readHead(inst, head.data(), (uInt32_t)head.size());
        const bool synth_header = fresh_header || (head.empty() && !raw);
        if (synth_header) {
            head = wav_header((uint64_t)info.noPoints * frame_bytes, info.samplerate, (uint16_t)info.channels,
                              (uint16_t)info.bitspersample, strncmp(info.sampleType, "FLOAT", 5) == 0);
        }
        ok = put(head, head.size());

        const uint32_t CHUNK = 65536;
        std::vector<uint8_t> buf((size_t)CHUNK * frame_bytes);
        long long done = 0;
        while (ok && done < info.noPoints) {
            uint32_t want = (uint32_t)std::min<long long>(CHUNK, info.noPoints - done);
            int got = OptimFROG_read(inst, buf.data(), want);
            if (got <= 0) break;
            ok = put(buf, (size_t)got * frame_bytes);
            done += got;
        }

        if (ok && !synth_header && done == info.noPoints) {
            sInt32_t tail_size = OptimFROG_readTail(inst, nullptr, 0);
            if (tail_size > 0) {
                std::vector<uint8_t> tail((size_t)tail_size);
                OptimFROG_readTail(inst, tail.data(), (uInt32_t)tail.size());
                ok = put(tail, tail.size());
            }
        }
        const bool damaged = OptimFROG_recoverableErrors(inst) != C_FALSE || done != info.noPoints;
        if (fclose(out) != 0) ok = false;
        if (!ok) { fprintf(stderr, "error: cannot write %s\n", fd.dst.c_str()); return 1; }
        if (damaged) fprintf(stderr, "warning: %s is damaged, the output is incomplete\n", fd.src.c_str());

        if (verbose) fprintf(stderr, "%s: rate=%u ch=%u bits=%u\n", fd.src.c_str(), info.samplerate, info.channels, info.bitspersample);
        if (!silent) fprintf(stderr, "%s -> %s (%lld frames)\n", fd.src.c_str(), fd.dst.c_str(), (long long)info.noPoints);
        if (show_time) {
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "%s: %.3fs\n", fd.src.c_str(), secs);
        }
        if (damaged) return 1;
    }
    return 0;
}

int do_info(const std::vector<std::string>& srcs) {
    printf("%-40s %6s %3s %5s %10s %8s\n", "Filename", "Rate", "Ch", "Bits", "Duration", "Points");
    for (const auto& src : srcs) {
        Instance inst;
        if (!inst || !OptimFROG_open(inst, src.c_str())) {
            fprintf(stderr, "error: cannot open %s\n", src.c_str());
            continue;
        }
        OptimFROG_Info info{};
        OptimFROG_getInfo(inst, &info);
        printf("%-40s %6u %3u %5u %9lldms %8lld\n", src.c_str(), info.samplerate, info.channels,
               info.bitspersample, (long long)info.length_ms, (long long)info.noPoints);
    }
    return 0;
}

} // namespace

static int run(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }

    enum { NONE, ENCODE, DECODE, INFO, VERIFY, CHECK, HELP } command = NONE;
    EncodeOptions opt;
    bool fresh_header = false;
    std::vector<SrcDst> files;
    std::vector<std::string> info_srcs;
    std::string pending_src;

    auto flush_pending = [&](const std::string& explicit_dst) {
        if (pending_src.empty()) return;
        if (command == INFO || command == VERIFY || command == CHECK) { info_srcs.push_back(pending_src); }
        else {
            std::string dst = explicit_dst.empty()
                ? default_output(pending_src, command == ENCODE ? ".ofr" : (opt.raw ? ".raw" : ".wav"))
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
        else if (a == "--verify") { command = VERIFY; }
        else if (a == "--check") { command = CHECK; }
        else if (a == "--help") { command = HELP; }
        else if (a == "--raw") { opt.raw = true; }
        else if (a == "--overwrite") { opt.overwrite = true; }
        else if (a == "--silent") { opt.silent = true; }
        else if (a == "--verbose") { opt.verbose = true; }
        else if (a == "--time") { opt.show_time = true; }
        else if (a == "--md5") { opt.want_md5 = true; }
        else if (a == "--writefreshheader") { fresh_header = true; }
        else if (a == "--preset" && i + 1 < argc) { opt.preset = argv[++i]; }
        else if (a == "--channelconfig" && i + 1 < argc) { opt.channelconfig = argv[++i]; }
        else if (a == "--sampletype" && i + 1 < argc) { opt.sampletype = argv[++i]; }
        else if (a == "--rate" && i + 1 < argc) { opt.rate = (uint32_t)atoi(argv[++i]); }
        else if (a == "--threads" && i + 1 < argc) { opt.threads = (uint32_t)atoi(argv[++i]); }
        else if (a == "--headersize" && i + 1 < argc) { opt.headersize = (uint32_t)atoi(argv[++i]); }
        else if (a == "--tailsize" && i + 1 < argc) { opt.tailsize = (uint32_t)atoi(argv[++i]); }
        else if (a == "--output" && i + 1 < argc) { flush_pending(argv[++i]); }
        else if (a.size() > 2 && a[0] == '-' && a[1] == '-') { fprintf(stderr, "warning: ignoring unrecognized option %s\n", a.c_str()); }
        else { flush_pending(""); pending_src = a; }
    }
    flush_pending("");

    if (command == HELP || command == NONE) { print_help(); return command == HELP ? 0 : 1; }
    if (command == ENCODE) return do_encode(files, opt);
    if (command == DECODE) return do_decode(files, opt.raw, fresh_header, opt.overwrite, opt.silent, opt.verbose, opt.show_time);
    if (command == INFO) return do_info(info_srcs);
    if (command == VERIFY) return do_verify(info_srcs, opt.verbose);
    if (command == CHECK) return do_check(info_srcs, opt.verbose);
    return 1;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    }
}
