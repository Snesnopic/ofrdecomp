/* C decoding API of ofrdecomp, source compatible with the interface of the official OptimFROG
   library so that existing callers can link against this implementation instead.
   Part of ofrdecomp, MIT licensed, see LICENSE. */

#ifndef OPTIMFROG_H_INCLUDED
#define OPTIMFROG_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

typedef int8_t sInt8_t;
typedef uint8_t uInt8_t;
typedef int16_t sInt16_t;
typedef uint16_t uInt16_t;
typedef int32_t sInt32_t;
typedef uint32_t uInt32_t;
typedef int64_t sInt64_t;
typedef uint64_t uInt64_t;
typedef float Float32_t;
typedef double Float64_t;

typedef unsigned char condition_t;
#define C_TRUE 1
#define C_FALSE 0
#define C_NULL NULL

/* byte packed, as in the official library */
#pragma pack(push, 1)

/* user supplied reader. close is called exactly once, when the decoder is done with the reader
   or fails to open it. */
typedef struct {
    condition_t (*close)(void* instance);
    sInt32_t (*read)(void* instance, void* destBuffer, uInt32_t count);
    condition_t (*eof)(void* instance);
    condition_t (*seekable)(void* instance);
    sInt64_t (*length)(void* instance);
    sInt64_t (*getPos)(void* instance);
    condition_t (*seek)(void* instance, sInt64_t pos);
} ReadInterface;

typedef struct {
    uInt32_t keyCount;
    char* keys[64];
    char* values[64];
} OptimFROG_Tags;

typedef struct {
    uInt32_t channels;
    uInt32_t samplerate;
    uInt32_t bitspersample;
    uInt32_t bitrate;

    uInt32_t version;
    const char* method;
    const char* speedup;

    sInt64_t noPoints;
    sInt64_t originalSize;
    sInt64_t compressedSize;
    sInt64_t length_ms;

    const char* sampleType;
    const char* channelConfig;
} OptimFROG_Info;

#pragma pack(pop)

typedef void (*OptimFROG_callBack)(void* callBackParam, Float64_t percentage);

#if defined(__cplusplus)
extern "C" {
#define OFR_DEFAULT_FALSE = C_FALSE
#define OFR_DEFAULT_NULL = C_NULL
#else
#define OFR_DEFAULT_FALSE
#define OFR_DEFAULT_NULL
#endif

uInt32_t OptimFROG_getVersion(void);

void* OptimFROG_createInstance(void);
void OptimFROG_destroyInstance(void* decoderInstance);

/* both return C_TRUE on success */
condition_t OptimFROG_openExt(void* decoderInstance, ReadInterface* rInt, void* readerInstance, condition_t readTags OFR_DEFAULT_FALSE);
condition_t OptimFROG_open(void* decoderInstance, const char* fileName, condition_t readTags OFR_DEFAULT_FALSE);
condition_t OptimFROG_close(void* decoderInstance);

/* copy the opaque bytes stored around the audio. the return value is the stored size: when it
   exceeds maxSize nothing is copied, so a first call with maxSize 0 queries the size.
   OptimFROG_readTail returns -1 until every point has been read. */
sInt32_t OptimFROG_readHead(void* decoderInstance, void* headData, uInt32_t maxSize);
sInt32_t OptimFROG_readTail(void* decoderInstance, void* tailData, uInt32_t maxSize);

condition_t OptimFROG_seekable(void* decoderInstance);
condition_t OptimFROG_seekPoint(void* decoderInstance, sInt64_t point);
condition_t OptimFROG_seekTime(void* decoderInstance, sInt64_t milliseconds);
sInt64_t OptimFROG_getPos(void* decoderInstance);
condition_t OptimFROG_recoverableErrors(void* decoderInstance);

/* a point is one sample per channel. returns the points read, 0 at end of stream, -1 on error.
   data receives the native sample format, bitspersample/8 bytes per value, or 16-bit values for
   wider formats when max16bit is set. */
sInt32_t OptimFROG_read(void* decoderInstance, void* data, uInt32_t noPoints, condition_t max16bit OFR_DEFAULT_FALSE);

condition_t OptimFROG_getInfo(void* decoderInstance, OptimFROG_Info* info);
condition_t OptimFROG_getTags(void* decoderInstance, OptimFROG_Tags* tags);
void OptimFROG_freeTags(OptimFROG_Tags* tags);

/* return one of the result codes below */
sInt32_t OptimFROG_decodeFile(const char* sourceFile, const char* destinationFile, OptimFROG_callBack callBack OFR_DEFAULT_NULL, void* callBackParam OFR_DEFAULT_NULL);
sInt32_t OptimFROG_infoFile(const char* sourceFile, OptimFROG_Info* info, OptimFROG_Tags* tags OFR_DEFAULT_NULL);

#if defined(__cplusplus)
}
#endif

#define OptimFROG_NoError 0
#define OptimFROG_MemoryError 1
#define OptimFROG_OpenError 2
#define OptimFROG_WriteError 3
#define OptimFROG_FatalError 4
#define OptimFROG_RecoverableError 5

#endif
