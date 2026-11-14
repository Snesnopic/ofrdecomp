// Modern C++ API over the decoder and encoder: namespaced, RAII, exceptions, std::span/vector.
// Works directly on the decoding engine, independently of the flat C API in OptimFROG.h.

/// \file ofrdecomp.hpp
/// \brief Namespaced, RAII, exception-based C++ API for the OptimFROG family of codecs.

#ifndef OFRDECOMP_HPP
#define OFRDECOMP_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/// \brief C++ API for OptimFROG streams (.ofr lossless, .off IEEE Float, .ofs DualStream).
namespace ofrdecomp {

/// \brief Thrown by Decoder on open/read failures.
///
/// what() carries a short human-readable description; code() is one of the OptimFROG_*
/// result codes (see OptimFROG.h) when known, otherwise -1.
class DecodeError : public std::runtime_error {
public:
    /// \param what human-readable description of the failure.
    /// \param code an OptimFROG_* result code (OptimFROG.h), or -1 if not applicable.
    DecodeError(const std::string& what, int code) : std::runtime_error(what), code_(code) {}
    ~DecodeError() override; // out-of-line: anchors the vtable to ofrdecomp.cpp

    /// \return the OptimFROG_* result code associated with this failure, or -1.
    int code() const noexcept { return code_; }

private:
    int code_;
};

/// \brief Thrown by the Encoder functions on invalid parameters.
class EncodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    ~EncodeError() override;
};

/// \brief Stream properties reported by Decoder::info().
struct Info {
    uint32_t channels = 0;      ///< Channel count (1 = mono, 2 = stereo).
    uint32_t samplerate = 0;    ///< Sample rate in Hz.
    uint32_t bitspersample = 0; ///< Bit depth of the original PCM (e.g. 16, 24).
    uint32_t bitrate = 0;       ///< Average bitrate in kbps.
    uint32_t version = 0;       ///< Version of the encoder that wrote the file.
    int64_t noPoints = 0;       ///< Total decodable length, in frames.
    int64_t originalSize = 0;   ///< Original (uncompressed) size, in bytes.
    int64_t compressedSize = 0; ///< Compressed file size, in bytes, 0 if the stream can't tell.
    int64_t length_ms = 0;      ///< Total length in milliseconds.
    bool isFloat = false;       ///< IEEE Float stream: samples are float32 bit patterns.
    std::string method;         ///< Encoder mode, as stored in the file.
    std::string speedup;        ///< Encoder speedup, as stored in the file.
    std::string sampleType;     ///< Sample format, e.g. "SINT16".
    std::string channelConfig;  ///< Channel layout, e.g. "STEREO_LR".
};

/// \brief A byte source for Decoder, for input that isn't a file or a memory buffer.
class Stream {
public:
    virtual ~Stream();

    /// \brief Reads up to `count` bytes into `dest`.
    /// \return the bytes read, 0 at end of stream.
    virtual size_t read(void* dest, size_t count) = 0;

    /// \return true if seek(), position() and length() work.
    virtual bool seekable() const { return false; }

    /// \return the total size in bytes.
    virtual int64_t length() const { return 0; }

    /// \return the current position in bytes.
    virtual int64_t position() const { return 0; }

    /// \brief Moves to an absolute byte position.
    /// \return true on success.
    virtual bool seek(int64_t) { return false; }
};

/// \brief Decodes one stream. Move-only: the object owns the decoder and its input.
///
/// Damage confined to a block doesn't throw: the block decodes to silence and
/// hadRecoverableErrors() turns true. The object isn't thread safe, but separate objects are
/// independent of each other.
class Decoder {
public:
    /// \brief Opens the file at `path`.
    /// \throws DecodeError if the file can't be opened or isn't a supported stream.
    explicit Decoder(const std::string& path);

    /// \brief Opens a stream held in memory, which must outlive the Decoder.
    /// \throws DecodeError if the data isn't a supported stream.
    explicit Decoder(std::span<const uint8_t> memory);

    /// \brief Opens a custom byte source.
    /// \throws DecodeError if the data isn't a supported stream.
    explicit Decoder(std::unique_ptr<Stream> stream);

    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&& other) noexcept;
    Decoder& operator=(Decoder&& other) noexcept;

    /// \return the stream's channel/rate/bit-depth/length metadata.
    Info info() const;

    /// \brief Decodes up to `frames` interleaved sample frames into `dest`.
    ///
    /// Integer samples come back sign-extended to 32 bits, float samples as their bit patterns.
    /// \param dest destination buffer, sized for at least `frames * info().channels` values.
    /// \param frames maximum number of frames to decode.
    /// \return the number of frames actually decoded (0 at end of stream).
    /// \throws DecodeError if `dest` is too small or the block structure is lost.
    size_t read(std::span<int32_t> dest, size_t frames);

    /// \brief Convenience helper that decodes the entire remaining stream in one call.
    /// \return one interleaved buffer holding every remaining sample frame.
    std::vector<int32_t> readAll();

    /// \return true if the input supports seeking.
    bool seekable() const;

    /// \brief Seeks to an absolute frame index, hopping over whole blocks.
    /// \param frame target frame index.
    /// \return true on success.
    bool seek(int64_t frame);

    /// \brief Seeks to an absolute time offset.
    /// \param milliseconds target offset from the start of the stream.
    /// \return true on success.
    bool seekTime(int64_t milliseconds);

    /// \return the current frame position.
    int64_t pos() const;

    /// \return true if a damaged block was found while decoding: its CRC didn't match, or it
    ///         couldn't be decoded and came out as silence.
    bool hadRecoverableErrors() const;

    /// \return the opaque bytes stored in front of the audio, usually the original WAV header.
    std::span<const uint8_t> head() const;

    /// \brief Reads the opaque bytes stored after the audio.
    /// \return the bytes, usually the end of the original WAV file.
    /// \throws DecodeError if the stream hasn't been read to its end, or has no tail block.
    std::span<const uint8_t> tail();

    /// \return the ID3v1 and APEv2 tags at the end of the input, as key/value pairs. Empty if
    ///         the input isn't seekable.
    std::vector<std::pair<std::string, std::string>> tags();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// \brief Encoder settings. The defaults are the fastest configuration.
struct EncodeOptions {
    int predictor = 1;          ///< 1 LPC, 2 cascade NLMS, 3 LPC + cascade NLMS.
    int entropy = 1;            ///< 1 fast, 2 slow, 3 context modeling.
    int monoOrder = 64;         ///< Mono LPC order, 1 to 127.
    int stereoOrderIndex = 1;   ///< Stereo LPC order, as an index into the format's order table.
    uint32_t blockFrames = 1u << 20; ///< Frames per independent block, 0 for as few as possible.
    uint32_t threads = 1;       ///< Blocks encoded in parallel.
    bool search = false;        ///< Per block, keep the smallest of a set of configurations.
    bool unsignedSamples = false; ///< Tag the stream as unsigned; samples still come in biased to signed.
};

/// \brief Stateless encode entry points. All of them are safe to call from several threads.
///
/// `head`/`tail` are opaque bytes embedded verbatim in the stream and handed back unchanged by
/// Decoder::head() and Decoder::tail().
namespace Encoder {
    /// \brief Encodes integer PCM to a lossless .ofr byte stream.
    /// \param samples signed PCM samples, interleaved, sign-extended to 32 bits.
    /// \param channels 1 or 2.
    /// \param samplerate sample rate in Hz.
    /// \param bitspersample bit depth of `samples` (8, 16, 24, or 32).
    /// \param options encoder settings.
    /// \param head bytes to embed verbatim in front of the audio.
    /// \param tail bytes to embed verbatim after the audio.
    /// \return the encoded file contents.
    /// \throws EncodeError on invalid parameters.
    std::vector<uint8_t> encode(std::span<const int32_t> samples, int channels, uint32_t samplerate, int bitspersample,
                                const EncodeOptions& options = {}, std::span<const uint8_t> head = {},
                                std::span<const uint8_t> tail = {});

    /// \brief Encodes float PCM to a lossless .off byte stream.
    /// \param samples float samples, interleaved.
    /// \param channels 1 or 2.
    /// \param samplerate sample rate in Hz.
    /// \param options encoder settings.
    /// \param head bytes to embed verbatim in front of the audio.
    /// \param tail bytes to embed verbatim after the audio.
    /// \return the encoded file contents.
    /// \throws EncodeError on invalid parameters.
    std::vector<uint8_t> encodeFloat(std::span<const float> samples, int channels, uint32_t samplerate,
                                     const EncodeOptions& options = {}, std::span<const uint8_t> head = {},
                                     std::span<const uint8_t> tail = {});
} // namespace Encoder

} // namespace ofrdecomp

#endif
