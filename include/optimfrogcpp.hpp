// Modern C++ facade over the flat C SDK (OptimFROG.h) and the encoder entry points
// (optimfrog_encoder.h). Purely additive: a thin wrapper, namespaced, RAII, exceptions,
// std::span/vector instead of manual create/destroy + raw pointers. Does not touch the
// bit-exact core -- see OptimFROG.h for the SDK-compatible flat API this project also exposes.
#ifndef OPTIMFROGCPP_HPP
#define OPTIMFROGCPP_HPP

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace optimfrogcpp {

// Thrown by Decoder on open/read failures. what() carries a short description; code() is one
// of the OptimFROG_* result codes (OptimFROG.h) when known, otherwise -1.
class DecodeError : public std::runtime_error {
public:
    DecodeError(const std::string& what, int code) : std::runtime_error(what), code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

struct Info {
    uint32_t channels = 0;
    uint32_t samplerate = 0;
    uint32_t bitspersample = 0;
    uint32_t bitrate = 0;
    int64_t noPoints = 0;       // frames
    int64_t originalSize = 0;   // bytes
    int64_t compressedSize = 0; // bytes
    int64_t length_ms = 0;
};

// RAII wrapper around OptimFROG_createInstance/open/read/close/destroyInstance. Move-only.
class Decoder {
public:
    explicit Decoder(const std::string& path); // throws DecodeError
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&& other) noexcept;
    Decoder& operator=(Decoder&& other) noexcept;

    Info info() const;

    // Reads up to `frames` interleaved sample frames into `dest` (sized for at least
    // frames * info().channels int32_t values). Returns frames actually read (0 at EOF).
    size_t read(std::span<int32_t> dest, size_t frames);

    // Convenience: decodes the entire remaining stream into one interleaved buffer.
    std::vector<int32_t> readAll();

    bool seekable() const;
    bool seek(int64_t frame);          // seek to an absolute frame index
    bool seekTime(int64_t milliseconds);
    int64_t pos() const;               // current frame position
    bool hadRecoverableErrors() const;

private:
    void* instance_ = nullptr;
};

// Stateless encode entry points, mirroring ofr_encode_mono/stereo (optimfrog_encoder.h).
// head/tail: opaque bytes embedded verbatim in the HEAD/TAIL container blocks, restored
// bit-exact around the decoded PCM (matches --headersize/--tailsize).
namespace Encoder {
    std::vector<uint8_t> encodeMono(std::span<const int32_t> samples, uint32_t samplerate, int bitspersample,
                                     std::span<const uint8_t> head = {}, std::span<const uint8_t> tail = {});
    std::vector<uint8_t> encodeStereo(std::span<const int32_t> interleaved, uint32_t samplerate, int bitspersample,
                                       std::span<const uint8_t> head = {}, std::span<const uint8_t> tail = {});
} // namespace Encoder

} // namespace optimfrogcpp

#endif
