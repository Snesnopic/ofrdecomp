// C API on top of OFR_DecoderEngine. return values, position handling and the file helpers follow
// the official library (exported functions at 0x1550-0x2a90 of libOptimFROG.0.dylib).
#include "../../include/OptimFROG.h"
#include "../../include/optimfrog_decoder.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#if defined(_WIN32)
#define ofr_fseek _fseeki64
#define ofr_ftell _ftelli64
#else
#define ofr_fseek fseeko
#define ofr_ftell ftello
#endif

namespace {

struct DecoderState {
    std::unique_ptr<OFR_DecoderEngine> engine;
    uint32_t bitrate = 0;
    int64_t noPoints = 0;
    int64_t originalSize = 0;
    int64_t compressedSize = 0;
    int64_t length_ms = 0;
    uint64_t position = 0;   // in values
    bool tail_read = false;
    bool has_tags = false;
    OFR_TagList tags;
    std::vector<int32_t> samples;
};

DecoderState* state_of(void* instance) { return static_cast<DecoderState*>(instance); }

condition_t file_close(void* instance) { return fclose((FILE*)instance) == 0 ? C_TRUE : C_FALSE; }

sInt32_t file_read(void* instance, void* buffer, uInt32_t size) {
    FILE* f = (FILE*)instance;
    size_t got = fread(buffer, 1, size, f);
    return (got == 0 && ferror(f)) ? -1 : (sInt32_t)got;
}

condition_t file_eof(void* instance) { return feof((FILE*)instance) ? C_TRUE : C_FALSE; }

condition_t file_seekable(void* instance) { return ofr_fseek((FILE*)instance, 0, SEEK_CUR) == 0 ? C_TRUE : C_FALSE; }

sInt64_t file_length(void* instance) {
    FILE* f = (FILE*)instance;
    sInt64_t pos = ofr_ftell(f), size = -1;
    if (pos >= 0 && ofr_fseek(f, 0, SEEK_END) == 0) {
        size = ofr_ftell(f);
        ofr_fseek(f, pos, SEEK_SET);
    }
    return size;
}

sInt64_t file_getPos(void* instance) { return ofr_ftell((FILE*)instance); }

condition_t file_seek(void* instance, sInt64_t pos) { return ofr_fseek((FILE*)instance, pos, SEEK_SET) == 0 ? C_TRUE : C_FALSE; }

ReadInterface file_interface = {file_close, file_read, file_eof, file_seekable, file_length, file_getPos, file_seek};

// the sample types of the container, unsigned ones stored with a bias
void pack_samples(const int32_t* src, uint32_t count, uint8_t* dst, uint32_t sample_type, bool max16bit) {
    const uint32_t bits = ofr_sample_bits(sample_type);
    const bool is_unsigned = sample_type < OFR_SAMPLE_FLOAT32 && (sample_type & 1) == 0;
    const uint32_t bias = is_unsigned ? 1u << (bits - 1) : 0u;
    const uint32_t bytes = bits / 8;
    const bool reduce = max16bit && bits > 16 && sample_type < OFR_SAMPLE_FLOAT32;
    const uint32_t skip = reduce ? bytes - 2 : 0, width = reduce ? 2 : bytes;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = (uint32_t)src[i] + bias;
        for (uint32_t b = 0; b < width; b++) *dst++ = (uint8_t)(v >> (8 * (b + skip)));
    }
}

} // namespace

OFR_DerivedInfo ofr_derived_info(OFR_DecoderEngine& e) {
    OFR_DerivedInfo d;
    d.noPoints = (int64_t)(e.total_samples / e.channels);
    d.originalSize = (int64_t)((e.bitspersample / 8) * e.total_samples + e.head.size());
    d.compressedSize = e.stream().seekable() ? e.stream().length() : 0;
    d.length_ms = trunc_to_int64_cvttsd2si((double)d.noPoints * 1000.0 / (double)e.samplerate);
    const double raw_rate = (double)(uint32_t)(e.samplerate * e.bitspersample * e.channels);
    if (d.originalSize != 0 && d.compressedSize != 0) {
        d.bitrate = (uint32_t)trunc_to_int64_cvttsd2si((double)d.compressedSize / (double)d.originalSize * raw_rate / 1000.0);
    } else {
        d.bitrate = (uint32_t)trunc_to_int64_cvttsd2si(raw_rate / 1000.0);
    }
    return d;
}

extern "C" {

uInt32_t OptimFROG_getVersion(void) {
    return 5100;
}

void* OptimFROG_createInstance(void) {
    return new (std::nothrow) DecoderState();
}

void OptimFROG_destroyInstance(void* decoderInstance) {
    delete state_of(decoderInstance);
}

condition_t OptimFROG_openExt(void* decoderInstance, ReadInterface* rInt, void* readerInstance, condition_t readTags) {
    DecoderState* s = state_of(decoderInstance);
    s->engine.reset();
    s->has_tags = false;
    s->tags.clear();

    try {
        s->engine = std::make_unique<OFR_DecoderEngine>(rInt, readerInstance);
    } catch (const std::bad_alloc&) {
        rInt->close(readerInstance);
        return C_FALSE;
    }

    OFR_DecoderEngine& e = *s->engine;
    try {
        e.open();
        if (readTags) {
            s->tags = ofr_read_tags(e.stream());
            s->has_tags = !s->tags.empty();
        }
    } catch (const std::exception&) {
        s->engine.reset();
        return C_FALSE;
    }

    const OFR_DerivedInfo derived = ofr_derived_info(e);
    s->noPoints = derived.noPoints;
    s->originalSize = derived.originalSize;
    s->compressedSize = derived.compressedSize;
    s->length_ms = derived.length_ms;
    s->bitrate = derived.bitrate;

    s->position = 0;
    s->tail_read = false;
    return C_TRUE;
}

condition_t OptimFROG_open(void* decoderInstance, const char* fileName, condition_t readTags) {
    FILE* file = fopen(fileName, "rb");
    if (!file) return C_FALSE;
    return OptimFROG_openExt(decoderInstance, &file_interface, file, readTags);
}

condition_t OptimFROG_close(void* decoderInstance) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine) return C_FALSE;
    s->engine.reset();
    s->has_tags = false;
    s->tags.clear();
    return C_TRUE;
}

sInt32_t OptimFROG_readHead(void* decoderInstance, void* headData, uInt32_t maxSize) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine) return 0;
    const std::vector<uint8_t>& head = s->engine->head;
    if (!head.empty() && head.size() <= maxSize) std::memcpy(headData, head.data(), head.size());
    return (sInt32_t)head.size();
}

sInt32_t OptimFROG_readTail(void* decoderInstance, void* tailData, uInt32_t maxSize) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine || s->position != s->engine->total_samples) return -1;

    if (!s->tail_read) {
        try {
            s->engine->read_tail();
        } catch (const std::exception&) {
            return -1;
        }
        s->tail_read = true;
    }

    const std::vector<uint8_t>& tail = s->engine->tail;
    if (!tail.empty() && tail.size() <= maxSize) std::memcpy(tailData, tail.data(), tail.size());
    return (sInt32_t)tail.size();
}

condition_t OptimFROG_seekable(void* decoderInstance) {
    DecoderState* s = state_of(decoderInstance);
    return (s && s->engine && s->engine->stream().seekable()) ? C_TRUE : C_FALSE;
}

condition_t OptimFROG_seekPoint(void* decoderInstance, sInt64_t point) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine || point < 0) return C_FALSE;

    uint64_t target = (uint64_t)point * s->engine->channels;
    bool ok = false;
    if (s->engine->stream().seekable() && target < s->engine->total_samples) {
        try {
            ok = s->engine->seek(target);
        } catch (const std::exception&) {
            ok = false;
        }
    }
    s->position = ok ? target : s->engine->total_samples;
    return ok ? C_TRUE : C_FALSE;
}

condition_t OptimFROG_seekTime(void* decoderInstance, sInt64_t milliseconds) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine) return C_FALSE;
    double point = (double)milliseconds / 1000.0 * (double)s->engine->samplerate;
    return OptimFROG_seekPoint(decoderInstance, trunc_to_int64_cvttsd2si(point));
}

sInt64_t OptimFROG_getPos(void* decoderInstance) {
    DecoderState* s = state_of(decoderInstance);
    return (s && s->engine) ? (sInt64_t)(s->position / s->engine->channels) : 0;
}

condition_t OptimFROG_recoverableErrors(void* decoderInstance) {
    DecoderState* s = state_of(decoderInstance);
    return (s && s->engine && s->engine->has_recoverable_errors) ? C_TRUE : C_FALSE;
}

sInt32_t OptimFROG_read(void* decoderInstance, void* data, uInt32_t noPoints, condition_t max16bit) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine) return -1;
    OFR_DecoderEngine& e = *s->engine;

    // one engine call for the whole request, as in the reference: after a decoding error the rest
    // of the slice turns to silence, so the slice size shows in the output of a damaged file. the
    // product is 32-bit in the reference too.
    const uint32_t want = (uint32_t)std::min<uint64_t>((uint32_t)(noPoints * e.channels), e.total_samples - s->position);
    uint32_t got = 0;
    try {
        if (s->samples.size() < want) s->samples.resize(want);
        got = e.read(s->samples.data(), want, false);
    } catch (const std::exception&) {
        return -1;
    }
    pack_samples(s->samples.data(), got, (uint8_t*)data, e.sample_type, max16bit != C_FALSE);
    s->position += got;
    if (s->samples.size() > (1u << 22)) std::vector<int32_t>().swap(s->samples);
    return (sInt32_t)(got / e.channels);
}

condition_t OptimFROG_getInfo(void* decoderInstance, OptimFROG_Info* info) {
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine) return C_FALSE;
    const OFR_DecoderEngine& e = *s->engine;

    info->channels = e.channels;
    info->samplerate = e.samplerate;
    info->bitspersample = e.bitspersample;
    info->bitrate = s->bitrate;
    info->version = e.version;
    info->method = ofr_method_name(e.method);
    info->speedup = ofr_speedup_name(e.speedup);
    info->noPoints = s->noPoints;
    info->originalSize = s->originalSize;
    info->compressedSize = s->compressedSize;
    info->length_ms = s->length_ms;
    info->sampleType = ofr_sample_type_name(e.sample_type);
    info->channelConfig = ofr_channel_config_name(e.channel_config);
    return C_TRUE;
}

condition_t OptimFROG_getTags(void* decoderInstance, OptimFROG_Tags* tags) {
    tags->keyCount = 0;
    DecoderState* s = state_of(decoderInstance);
    if (!s || !s->engine || !s->has_tags) return C_FALSE;

    for (const auto& tag : s->tags) {
        char* key = (char*)std::malloc(tag.first.size() + 1);
        char* value = (char*)std::malloc(tag.second.size() + 1);
        if (!key || !value) {
            std::free(key);
            std::free(value);
            break;
        }
        std::memcpy(key, tag.first.c_str(), tag.first.size() + 1);
        std::memcpy(value, tag.second.c_str(), tag.second.size() + 1);
        tags->keys[tags->keyCount] = key;
        tags->values[tags->keyCount] = value;
        tags->keyCount++;
    }
    return C_TRUE;
}

void OptimFROG_freeTags(OptimFROG_Tags* tags) {
    if (!tags) return;
    for (uInt32_t i = 0; i < tags->keyCount; ++i) {
        std::free(tags->keys[i]);
        tags->keys[i] = nullptr;
        std::free(tags->values[i]);
        tags->values[i] = nullptr;
    }
    tags->keyCount = 0;
}

sInt32_t OptimFROG_decodeFile(const char* sourceFile, const char* destinationFile, OptimFROG_callBack callBack, void* callBackParam) {
    std::unique_ptr<void, decltype(&OptimFROG_destroyInstance)> decoder(OptimFROG_createInstance(), OptimFROG_destroyInstance);
    if (!decoder) return OptimFROG_MemoryError;
    void* inst = decoder.get();

    if (!OptimFROG_open(inst, sourceFile, C_FALSE)) return OptimFROG_OpenError;

    OptimFROG_Info info{};
    OptimFROG_getInfo(inst, &info);

    std::unique_ptr<FILE, int (*)(FILE*)> out(fopen(destinationFile, "wb"), fclose);
    if (!out) return OptimFROG_WriteError;

    auto write_bytes = [&out](const std::vector<uint8_t>& bytes, size_t n) {
        return n == 0 || fwrite(bytes.data(), 1, n, out.get()) == n;
    };

    try {
        // one second of 44.1 kHz stereo 16-bit, the chunk the official library works in
        std::vector<uint8_t> buffer(0x2b110);

        size_t head_size = (size_t)OptimFROG_readHead(inst, nullptr, 0);
        if (head_size > buffer.size()) buffer.resize(head_size);
        OptimFROG_readHead(inst, buffer.data(), (uInt32_t)buffer.size());
        if (!write_bytes(buffer, head_size)) return OptimFROG_WriteError;

        const uint32_t bytes_per_point = (info.bitspersample / 8) * info.channels;
        const uint32_t points_per_chunk = 0x2b110 / bytes_per_point;
        sInt64_t points_read = 0;
        while (points_read < info.noPoints) {
            if (callBack) callBack(callBackParam, (double)points_read * 100.0 / (double)info.noPoints);
            sInt32_t got = OptimFROG_read(inst, buffer.data(), points_per_chunk, C_FALSE);
            if (got < 1) return OptimFROG_FatalError;
            if (!write_bytes(buffer, (size_t)got * bytes_per_point)) return OptimFROG_WriteError;
            points_read += got;
        }

        sInt32_t tail_size = OptimFROG_readTail(inst, nullptr, 0);
        if (tail_size < 0) return OptimFROG_FatalError;
        if ((size_t)tail_size > buffer.size()) buffer.resize((size_t)tail_size);
        OptimFROG_readTail(inst, buffer.data(), (uInt32_t)buffer.size());
        if (!write_bytes(buffer, (size_t)tail_size)) return OptimFROG_WriteError;
    } catch (const std::bad_alloc&) {
        return OptimFROG_MemoryError;
    }

    bool damaged = OptimFROG_recoverableErrors(inst) != C_FALSE;
    if (fclose(out.release()) != 0) return OptimFROG_WriteError;
    return damaged ? OptimFROG_RecoverableError : OptimFROG_NoError;
}

sInt32_t OptimFROG_infoFile(const char* sourceFile, OptimFROG_Info* info, OptimFROG_Tags* tags) {
    std::unique_ptr<void, decltype(&OptimFROG_destroyInstance)> decoder(OptimFROG_createInstance(), OptimFROG_destroyInstance);
    if (!decoder) return OptimFROG_MemoryError;
    void* inst = decoder.get();

    if (!OptimFROG_open(inst, sourceFile, tags != nullptr)) return OptimFROG_OpenError;
    OptimFROG_getInfo(inst, info);
    if (tags) OptimFROG_getTags(inst, tags);
    return OptimFROG_NoError;
}

} // extern "C"
