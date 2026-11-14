#include "../../include/ofrdecomp.hpp"
#include "../../include/OptimFROG.h"
#include "../../include/optimfrog_decoder.h"
#include "../../include/optimfrog_encoder.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define ofr_fseek _fseeki64
#define ofr_ftell _ftelli64
#else
#define ofr_fseek fseeko
#define ofr_ftell ftello
#endif

namespace ofrdecomp {

DecodeError::~DecodeError() = default;
EncodeError::~EncodeError() = default;
Stream::~Stream() = default;

namespace {

class FileStream : public Stream {
public:
    explicit FileStream(const std::string& path) : file_(fopen(path.c_str(), "rb")) {
        if (!file_) throw DecodeError("cannot open " + path, OptimFROG_OpenError);
    }
    ~FileStream() override { fclose(file_); }
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    size_t read(void* dest, size_t count) override { return fread(dest, 1, count, file_); }
    bool seekable() const override { return ofr_fseek(file_, 0, SEEK_CUR) == 0; }
    int64_t position() const override { return ofr_ftell(file_); }
    bool seek(int64_t pos) override { return ofr_fseek(file_, pos, SEEK_SET) == 0; }
    int64_t length() const override {
        int64_t pos = ofr_ftell(file_), size = 0;
        if (pos >= 0 && ofr_fseek(file_, 0, SEEK_END) == 0) {
            size = ofr_ftell(file_);
            ofr_fseek(file_, pos, SEEK_SET);
        }
        return size;
    }

private:
    FILE* file_;
};

class MemoryStream : public Stream {
public:
    explicit MemoryStream(std::span<const uint8_t> data) : data_(data) {}

    size_t read(void* dest, size_t count) override {
        size_t n = std::min(count, data_.size() - pos_);
        if (n) std::memcpy(dest, data_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    bool seekable() const override { return true; }
    int64_t length() const override { return (int64_t)data_.size(); }
    int64_t position() const override { return (int64_t)pos_; }
    bool seek(int64_t pos) override {
        if (pos < 0 || (uint64_t)pos > data_.size()) return false;
        pos_ = (size_t)pos;
        return true;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

// the engine's reader interface, forwarding to a Stream
Stream* stream_of(void* instance) { return static_cast<Stream*>(instance); }

ReadInterface stream_interface = {
    [](void*) -> condition_t { return C_TRUE; },
    [](void* s, void* dest, uInt32_t count) -> sInt32_t { return (sInt32_t)stream_of(s)->read(dest, count); },
    [](void*) -> condition_t { return C_FALSE; },
    [](void* s) -> condition_t { return stream_of(s)->seekable() ? C_TRUE : C_FALSE; },
    [](void* s) -> sInt64_t { return stream_of(s)->length(); },
    [](void* s) -> sInt64_t { return stream_of(s)->position(); },
    [](void* s, sInt64_t pos) -> condition_t { return stream_of(s)->seek(pos) ? C_TRUE : C_FALSE; },
};

OFR_EncodeParams to_params(const EncodeOptions& o) {
    OFR_EncodeParams p;
    p.pred = o.predictor;
    p.ent = o.entropy;
    p.mono_order = o.monoOrder;
    p.stereo_order_idx = o.stereoOrderIndex;
    p.block_frames = o.blockFrames;
    p.threads = o.threads;
    p.search = o.search;
    p.unsigned_samples = o.unsignedSamples;
    return p;
}

} // namespace

struct Decoder::Impl {
    std::unique_ptr<Stream> stream;
    OFR_DecoderEngine engine;
    bool tail_read = false;

    explicit Impl(std::unique_ptr<Stream> s) : stream(std::move(s)), engine(&stream_interface, stream.get()) {
        try {
            engine.open();
        } catch (const OFR_StreamError& e) {
            throw DecodeError(e.what(), OptimFROG_OpenError);
        }
    }
};

Decoder::Decoder(const std::string& path) : impl_(std::make_unique<Impl>(std::make_unique<FileStream>(path))) {}
Decoder::Decoder(std::span<const uint8_t> memory) : impl_(std::make_unique<Impl>(std::make_unique<MemoryStream>(memory))) {}
Decoder::Decoder(std::unique_ptr<Stream> stream) : impl_(std::make_unique<Impl>(std::move(stream))) {}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&& other) noexcept = default;
Decoder& Decoder::operator=(Decoder&& other) noexcept = default;

Info Decoder::info() const {
    OFR_DecoderEngine& e = impl_->engine;
    Info out;
    out.channels = e.channels;
    out.samplerate = e.samplerate;
    out.bitspersample = e.bitspersample;
    out.version = e.version;
    const OFR_DerivedInfo derived = ofr_derived_info(e);
    out.noPoints = derived.noPoints;
    out.originalSize = derived.originalSize;
    out.compressedSize = derived.compressedSize;
    out.length_ms = derived.length_ms;
    out.bitrate = derived.bitrate;
    out.isFloat = e.is_float;
    out.method = ofr_method_name(e.method);
    out.speedup = ofr_speedup_name(e.speedup);
    out.sampleType = ofr_sample_type_name(e.sample_type);
    out.channelConfig = ofr_channel_config_name(e.channel_config);
    return out;
}

size_t Decoder::read(std::span<int32_t> dest, size_t frames) {
    OFR_DecoderEngine& e = impl_->engine;
    if (dest.size() / e.channels < frames) throw DecodeError("Decoder::read: destination span too small", -1);

    uint64_t remaining = std::min<uint64_t>((uint64_t)frames * e.channels, e.total_samples - e.position);
    size_t done = 0;
    try {
        while (remaining > 0) {
            uint32_t want = (uint32_t)std::min<uint64_t>(remaining, 1u << 30);
            uint32_t got = e.read(dest.data() + done, want);
            if (got == 0) break;
            done += got;
            remaining -= got;
        }
    } catch (const OFR_StreamError& err) {
        if (done == 0) throw DecodeError(err.what(), OptimFROG_FatalError);
    }
    return done / e.channels;
}

std::vector<int32_t> Decoder::readAll() {
    OFR_DecoderEngine& e = impl_->engine;
    std::vector<int32_t> out;
    constexpr size_t CHUNK = 1u << 16;
    while (e.position < e.total_samples) {
        size_t base = out.size();
        out.resize(base + CHUNK * e.channels);
        size_t got = read(std::span<int32_t>(out).subspan(base), CHUNK);
        out.resize(base + got * e.channels);
        if (got == 0) break;
    }
    return out;
}

bool Decoder::seekable() const { return impl_->engine.stream().seekable(); }

bool Decoder::seek(int64_t frame) {
    if (frame < 0) return false;
    try {
        return impl_->engine.seek((uint64_t)frame * impl_->engine.channels);
    } catch (const OFR_StreamError&) {
        return false;
    }
}

bool Decoder::seekTime(int64_t milliseconds) {
    return seek(trunc_to_int64_cvttsd2si((double)milliseconds / 1000.0 * (double)impl_->engine.samplerate));
}

int64_t Decoder::pos() const { return (int64_t)(impl_->engine.position / impl_->engine.channels); }

bool Decoder::hadRecoverableErrors() const { return impl_->engine.has_recoverable_errors; }

std::span<const uint8_t> Decoder::head() const { return impl_->engine.head; }

std::span<const uint8_t> Decoder::tail() {
    OFR_DecoderEngine& e = impl_->engine;
    if (e.position != e.total_samples) throw DecodeError("Decoder::tail: the stream hasn't been read to its end", -1);
    if (!impl_->tail_read) {
        try {
            e.read_tail();
        } catch (const OFR_StreamError& err) {
            throw DecodeError(err.what(), OptimFROG_FatalError);
        }
        impl_->tail_read = true;
    }
    return e.tail;
}

std::vector<std::pair<std::string, std::string>> Decoder::tags() { return ofr_read_tags(impl_->engine.stream()); }

namespace Encoder {

std::vector<uint8_t> encode(std::span<const int32_t> samples, int channels, uint32_t samplerate, int bitspersample,
                            const EncodeOptions& options, std::span<const uint8_t> head, std::span<const uint8_t> tail) {
    if (channels != 1 && channels != 2) throw EncodeError("channels must be 1 or 2");
    std::vector<uint8_t> file;
    std::vector<uint8_t> h(head.begin(), head.end()), t(tail.begin(), tail.end());
    if (!ofr_encode(samples.data(), samples.size() / (size_t)channels, channels, samplerate, bitspersample,
                    to_params(options), file, h, t))
        throw EncodeError("invalid encoder parameters");
    return file;
}

std::vector<uint8_t> encodeFloat(std::span<const float> samples, int channels, uint32_t samplerate,
                                 const EncodeOptions& options, std::span<const uint8_t> head, std::span<const uint8_t> tail) {
    if (channels != 1 && channels != 2) throw EncodeError("channels must be 1 or 2");
    std::vector<uint8_t> file;
    std::vector<uint8_t> h(head.begin(), head.end()), t(tail.begin(), tail.end());
    if (!ofr_encode_float(samples.data(), samples.size() / (size_t)channels, channels, samplerate, to_params(options), file, h, t))
        throw EncodeError("invalid encoder parameters");
    return file;
}

} // namespace Encoder
} // namespace ofrdecomp
