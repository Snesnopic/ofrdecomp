#include "../../include/optimfrog_decoder.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr size_t READ_BUFFER = 1u << 16;
constexpr uint32_t MAGIC_SCAN_LIMIT = 0x100000;   // self-extracting files carry a stub in front
constexpr uint32_t MAX_DECODER_VERSION = 5100;

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

OFR_BitStream::OFR_BitStream(ReadInterface* rInt, void* readerInstance)
    : rInt_(rInt), inst_(readerInstance), buffer_(READ_BUFFER) {
    if (seekable()) {
        sInt64_t p = rInt_->getPos(inst_);
        pos_ = p > 0 ? (uint64_t)p : 0;
    }
}

size_t OFR_BitStream::read(void* dst, size_t count) {
    uint8_t* d = (uint8_t*)dst;
    size_t done = 0;
    while (done < count) {
        if (buf_pos_ == buf_avail_) {
            sInt32_t got = open_ ? rInt_->read(inst_, buffer_.data(), (uInt32_t)buffer_.size()) : 0;
            buf_pos_ = 0;
            buf_avail_ = got > 0 ? (size_t)got : 0;
            if (buf_avail_ == 0) break;
        }
        size_t n = std::min(count - done, buf_avail_ - buf_pos_);
        std::memcpy(d + done, buffer_.data() + buf_pos_, n);
        buf_pos_ += n;
        done += n;
    }
    pos_ += done;
    return done;
}

uint32_t OFR_BitStream::peek_tag() {
    uint8_t b[4] = {0, 0, 0, 0};
    read(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void OFR_BitStream::read_exact(void* dst, size_t count) {
    if (read(dst, count) != count) throw OFR_StreamError("unexpected end of stream");
}

size_t OFR_BitStream::read_append(std::vector<uint8_t>& dst, uint64_t count) {
    size_t total = 0;
    while (count > 0) {
        size_t n = (size_t)std::min<uint64_t>(count, 1u << 20);
        size_t base = dst.size();
        dst.resize(base + n);
        size_t got = read(dst.data() + base, n);
        dst.resize(base + got);
        total += got;
        count -= got;
        if (got < n) break;
    }
    return total;
}

uint16_t OFR_BitStream::read_u16() {
    uint8_t b[2];
    read_exact(b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

uint32_t OFR_BitStream::read_u32() {
    uint8_t b[4];
    read_exact(b, 4);
    return le32(b);
}

bool OFR_BitStream::seek(uint64_t byte_pos) {
    if (!open_ || !rInt_->seek(inst_, (sInt64_t)byte_pos)) return false;
    buf_pos_ = 0;
    buf_avail_ = 0;
    pos_ = byte_pos;
    return true;
}

bool OFR_BitStream::skip(uint64_t count) {
    if (seekable()) return seek(pos_ + count);
    uint8_t tmp[4096];
    while (count > 0) {
        size_t n = (size_t)std::min<uint64_t>(count, sizeof(tmp));
        if (read(tmp, n) != n) return false;
        count -= n;
    }
    return true;
}

bool OFR_BitStream::seekable() const {
    return open_ && rInt_->seekable(inst_) != C_FALSE;
}

int64_t OFR_BitStream::length() const {
    return (open_ && rInt_->length) ? rInt_->length(inst_) : 0;
}

void OFR_BitStream::close() {
    if (open_) {
        open_ = false;
        rInt_->close(inst_);
    }
}

void OFR_RangeCoder::init(const uint8_t* data, size_t size, OFR_BitStream* spill_from, uint64_t limit) {
    cur = data;
    end = data + size;
    spill = spill_from;
    overrun = 0;
    overrun_limit = limit;
    next_byte();   // the encoder's dummy first byte
    cache = next_byte();
    range = 0x80;
    value = cache >> 1;
    normalize();
}

uint8_t OFR_RangeCoder::next_byte_past_end() {
    uint8_t b;
    if (spill && spill->read(&b, 1) == 1) return b;
    if (++overrun > overrun_limit) throw OFR_StreamError("range coder ran past the end of the stream");
    return 0;
}

// FUN_00016f10 / FUN_000173b0 (post=1 coded-range reduction). the reference picks between two
// precisions depending on whether every channel's min/max fit a safe +-2^23 range:
//   - narrow: 32-bit arithmetic, mult as its native SIGNED int32 value.
//   - wide:   64-bit arithmetic, mult zero-extended to an UNSIGNED value.
// the min result rounds down and the max result rounds up. mult and offset come from the stream,
// so the narrow division goes through 64 bits, where INT32_MIN / -1 can't trap.
static void ofr_reduce_range(bool wide, int min_val, int max_val, int offset, int mult, int& out_min, int& out_max) {
    if (mult == 0) throw OFR_StreamError("post-processor multiplier is zero");
    int64_t divisor, a_min, a_max;
    if (wide) {
        divisor = (int64_t)(uint32_t)mult;
        a_min = (int64_t)min_val - (int64_t)offset;
        a_max = (int64_t)max_val - (int64_t)offset;
    } else {
        divisor = mult;
        a_min = (int32_t)((uint32_t)min_val - (uint32_t)offset);
        a_max = (int32_t)((uint32_t)max_val - (uint32_t)offset);
    }
    int64_t q_min = a_min / divisor, r_min = a_min % divisor;
    out_min = (int32_t)(q_min + (r_min < 0 ? -1 : 0));
    int64_t q_max = a_max / divisor, r_max = a_max % divisor;
    out_max = (int32_t)(r_max > 0 ? q_max + 1 : q_max);
}

void OFR_PostProcessor::read_channel(OFR_RangeCoder* rc, uint32_t bit_depth, int& mn, int& mx, int& mult, int& offset) {
    auto sign_extend = [bit_depth](uint32_t val) {
        uint32_t shift = 32 - bit_depth;
        return (int32_t)(val << shift) >> shift;
    };
    mn = sign_extend(rc->read_uniform_split(bit_depth));
    mx = sign_extend(rc->read_uniform_split(bit_depth));
    if (rc->read_uniform_bits(1) == 0) {
        mult = 1;
        offset = 0;
    } else {
        mult = (int)rc->read_uniform_split(bit_depth);
        offset = sign_extend(rc->read_uniform_split(bit_depth));
    }
}

void OFR_PostProcessor::init(OFR_RangeCoder* rc, uint32_t bit_depth, uint32_t channels, bool clamp) {
    m_channels = channels;
    has_remap = false;
    full_range_clamp = clamp;

    read_channel(rc, bit_depth, min_val_L, max_val_L, mult_L, offset_L);
    if (channels == 2) read_channel(rc, bit_depth, min_val_R, max_val_R, mult_R, offset_R);

    auto is_wide = [this, channels] {
        int lo = channels == 2 ? std::min(min_val_L, min_val_R) : min_val_L;
        int hi = channels == 2 ? std::max(max_val_L, max_val_R) : max_val_L;
        return lo < -0x800000 || hi >= 0x800000;
    };

    wide = is_wide();
    ofr_reduce_range(wide, min_val_L, max_val_L, offset_L, mult_L, scaled_min_L, scaled_max_L);
    if (channels == 2) ofr_reduce_range(wide, min_val_R, max_val_R, offset_R, mult_R, scaled_min_R, scaled_max_R);

    if (clamp) {
        uint32_t half = 1u << (bit_depth - 1);
        min_val_L = min_val_R = (int32_t)(0u - half);
        max_val_L = max_val_R = (int32_t)(half - 1u);
        wide = is_wide();
    }
}

void OFR_BlockDecoder::init_block(OFR_RangeCoder& rc, uint32_t reserved, uint32_t channels, uint32_t bits,
                                  uint32_t values) {
    post_type_ = pred_type_ = entropy_type_ = 0;
    channels_ = channels;

    const uint32_t post_type = reserved & 0x3f;
    const uint32_t pred_type = (reserved >> 6) & 0x1f;
    const uint32_t entropy_type = reserved >> 11;
    const bool stereo = channels == 2;

    if (post_type != 1 && post_type != 2) throw OFR_StreamError("invalid post-processor type");
    if (!post_) post_ = std::make_unique<OFR_PostProcessor>();
    if (post_type == 2) {
        post_->init2(&rc, channels, bits);
    } else {
        post_->init(&rc, bits, channels, pred_type == 4);
    }

    // the predictor and the entropy coder work on the coded range of the post-processor
    OFR_PostProcessor& pp = *post_;
    const int mnL = pp.scaled_min_L, mxL = pp.scaled_max_L, mnR = pp.scaled_min_R, mxR = pp.scaled_max_R;
    const int data_bits = stereo ? ofr_bitlen(std::min(mnL, mnR), std::max(mxL, mxR)) : ofr_bitlen(mnL, mxL);

    switch (pred_type) {
    case 1:
        if (stereo) {
            if (!predictor_stereo_) predictor_stereo_ = std::make_unique<OFR_PredictorStereo>();
            predictor_stereo_->init(&rc);
            predictor_stereo_->m_left_min = mnL;
            predictor_stereo_->m_left_max = mxL;
            predictor_stereo_->m_right_min = mnR;
            predictor_stereo_->m_right_max = mxR;
            predictor_stereo_->m_shift = 32u - (uint32_t)data_bits;
        } else {
            if (!predictor_) predictor_ = std::make_unique<OFR_Predictor>();
            predictor_->init(&rc);
            predictor_->min_val = mnL;
            predictor_->max_val = mxL;
            predictor_->shift = 32 - data_bits;
        }
        break;
    case 2:
    case 3:
        if (stereo) {
            if (!cascade_stereo_) cascade_stereo_ = std::make_unique<OFR_PredictorCascadeStereo>();
            if (pred_type == 2) cascade_stereo_->init_cascade_only(&rc, mnL, mxL, mnR, mxR, data_bits);
            else cascade_stereo_->init(&rc, mnL, mxL, mnR, mxR, data_bits, (int)values);
        } else {
            if (!cascade_mono_) cascade_mono_ = std::make_unique<OFR_PredictorCascadeMono>();
            if (pred_type == 2) cascade_mono_->init_cascade_only(&rc, mnL, mxL, data_bits);
            else cascade_mono_->init(&rc, mnL, mxL, data_bits, (int)values);
        }
        break;
    case 4:
        if (stereo) {
            if (!dualstream_stereo_) dualstream_stereo_ = std::make_unique<OFR_PredictorDualStreamStereo>();
            dualstream_stereo_->init(&rc, data_bits, values);
        } else {
            if (!dualstream_mono_) dualstream_mono_ = std::make_unique<OFR_PredictorDualStreamMono>();
            dualstream_mono_->init(&rc, data_bits, values);
        }
        break;
    default:
        throw OFR_StreamError("invalid predictor type");
    }

    if (entropy_type == 3) {
        if (!entropy_acm_) entropy_acm_ = std::make_unique<OFR_EntropyAcm>();
        entropy_acm_->init(&rc, (uint32_t)data_bits, channels);
    } else if (entropy_type == 1 || entropy_type == 2) {
        if (!entropy_) entropy_ = std::make_unique<OFR_EntropyDecoder>();
        entropy_->init((uint32_t)data_bits, channels, entropy_type);
        entropy_->init_from_bitstream(&rc);
    } else {
        throw OFR_StreamError("invalid entropy coder type");
    }

    post_type_ = post_type;
    pred_type_ = pred_type;
    entropy_type_ = entropy_type;
}

void OFR_BlockDecoder::decode_block(int32_t* dest, uint32_t count, OFR_RangeCoder& rc) {
    if (entropy_type_ == 3) entropy_acm_->decode_block(dest, count, &rc);
    else entropy_->decode_block(dest, count, &rc);

    const bool stereo = channels_ == 2;
    switch (pred_type_) {
    case 1:
        if (stereo) predictor_stereo_->decode(dest, count);
        else predictor_->decode(dest, count);
        break;
    case 2:
    case 3:
        if (stereo) cascade_stereo_->decode(dest, count);
        else cascade_mono_->decode(dest, count);
        break;
    default:
        if (stereo) dualstream_stereo_->decode(dest, count);
        else dualstream_mono_->decode(dest, count);
        break;
    }

    post_->decode(dest, count);
}

OFR_DecoderEngine::OFR_DecoderEngine(ReadInterface* rInt, void* readerInstance) : bs_(rInt, readerInstance) {}

OFR_DecoderEngine::~OFR_DecoderEngine() {
    close();
}

void OFR_DecoderEngine::close() {
    bs_.close();
}

// the main block starts with "OFR " (or "OFRX" for IEEE Float) followed by a sane block size. a
// self-extracting file has its decoder stub in front of that, so look a little further.
void OFR_DecoderEngine::open() {
    try {
        uint8_t w[8];
        bs_.read_exact(w, 8);
        if (std::memcmp(w, "*RIF", 4) == 0) throw OFR_StreamError("OFR 4.5alpha file");

        uint32_t scanned = 0;
        uint32_t magic = le32(w), size = le32(w + 4);
        while (!((magic == OFR_TAG_OFR || magic == OFR_TAG_OFRX) && size >= 0xc && size < 0x100)) {
            if (++scanned >= MAGIC_SCAN_LIMIT) throw OFR_StreamError("not an OFR file");
            std::memmove(w, w + 1, 7);
            w[7] = bs_.read_u8();
            magic = le32(w);
            size = le32(w + 4);
        }
        is_float = magic == OFR_TAG_OFRX;

        if (size == 0xc) throw OFR_StreamError("OFR 4.5alpha file");
        if (size < 0xf || size >= 0x50) throw OFR_StreamError("invalid OFR file");

        uint32_t ts_low = bs_.read_u32();
        uint16_t ts_high = bs_.read_u16();
        total_samples = ((uint64_t)ts_high << 32) | ts_low;
        sample_type = bs_.read_u8();
        channel_config = bs_.read_u8();
        samplerate = bs_.read_u32();

        uint16_t encoder_id = bs_.read_u16();
        uint8_t compression = bs_.read_u8();
        version = (encoder_id >> 4) + 0x1194;
        speedup = compression & 7;
        method = compression >> 3;

        uint32_t consumed = 0xf;
        if (size >= 0x11) {
            uint32_t min_decoder_version = bs_.read_u16() + 0x1194u;
            consumed = 0x11;
            if (min_decoder_version > MAX_DECODER_VERSION) throw OFR_StreamError("unsupported newer version OFR file");
        }

        if (channel_config >= 2) throw OFR_StreamError("channels > 2");
        if (sample_type >= OFR_SAMPLE_TYPE_COUNT) throw OFR_StreamError("invalid or unsupported newer version OFR file");
        channels = channel_config + 1;
        bitspersample = ofr_sample_bits(sample_type);

        if (!bs_.skip(size - consumed)) throw OFR_StreamError("unexpected end of stream");

        if (bs_.read_u32() != OFR_TAG_HEAD) throw OFR_StreamError("invalid OFR file");
        uint32_t head_size = bs_.read_u32();
        if (bs_.read_append(head, head_size) != head_size) throw OFR_StreamError("unexpected end of stream");

        first_block_pos_ = next_block_pos_ = bs_.tell();
    } catch (...) {
        bs_.close();
        throw;
    }
}

void OFR_DecoderEngine::remember_block(uint64_t offset, uint64_t value_pos, uint32_t values) {
    if (index_.empty() ? value_pos == 0 : index_.back().value_pos + index_.back().values == value_pos) {
        index_.push_back({offset, value_pos, values});
    }
}

// reads the next COMP block into memory and sets up its decoders. a bad block leaves
// block_error_ set, so the caller emits silence for its length and carries on with the next one
// (FUN at 0x12260 in the library); only losing the block structure itself is fatal.
void OFR_DecoderEngine::next_block() {
    block_error_ = false;

    uint64_t block_pos = bs_.tell();
    uint32_t magic = bs_.peek_tag();
    if (magic != OFR_TAG_COMP) {
        has_recoverable_errors = true;
        if (!bs_.seek(next_block_pos_)) throw OFR_StreamError("lost the block structure");
        block_pos = next_block_pos_;
        if (bs_.read_u32() != OFR_TAG_COMP) throw OFR_StreamError("lost the block structure");
    }

    uint32_t comp_size = bs_.read_u32();
    uint32_t stored_crc = bs_.read_u32();
    if (comp_size < 13) throw OFR_StreamError("invalid COMP block size");
    next_block_pos_ = block_pos + 8 + comp_size;

    const uint64_t body_size = comp_size - 4;
    payload_.clear();
    uint64_t got = bs_.read_append(payload_, body_size);
    if (got < 9) throw OFR_StreamError("unexpected end of stream");
    if (got < body_size) has_recoverable_errors = true;

    // the reference doesn't look at the CRC while decoding and neither does this decide anything:
    // it only makes damage visible in the flag, the block is still decoded the same way
    if (got == body_size && ofr_crc32(payload_.data(), payload_.size()) != stored_crc) has_recoverable_errors = true;

    const uint8_t* p = payload_.data();
    block_values_ = le32(p);
    block_pos_ = 0;
    remember_block(block_pos, position, block_values_);

    const uint32_t block_sample_type = p[4], block_channel_config = p[5];
    const uint32_t reserved = (uint32_t)p[6] | ((uint32_t)p[7] << 8);

    std::vector<uint8_t> deta;
    bool deta_ok = true;
    if (is_float) {
        if (bs_.read_u32() == OFR_TAG_DETA) {
            uint32_t deta_size = bs_.read_u32();
            bs_.read_u32();   // crc32
            uint64_t deta_body = deta_size >= 4 ? deta_size - 4 : 0;
            next_block_pos_ = bs_.tell() + deta_body;
            deta_ok = bs_.read_append(deta, deta_body) == deta_body;
        } else {
            deta_ok = false;
        }
    }

    try {
        if (block_sample_type != sample_type || block_channel_config != channel_config)
            throw OFR_StreamError("block format differs from the file");
        if (block_values_ > total_samples - position || block_values_ > (uint32_t)INT32_MAX || block_values_ % channels != 0)
            throw OFR_StreamError("invalid block length");
        if (!deta_ok) throw OFR_StreamError("damaged DETA sub-block");

        // integer blocks only: a float block's DETA sub-block sits in between, and the float
        // decoder, a separate binary, wasn't checked for this
        // a value takes well under 8 bytes even at 32 bits
        range_coder_.init(p + 9, payload_.size() - 9, is_float ? nullptr : &bs_,
                          OFR_RangeCoder::MAX_OVERRUN + 8ull * block_values_);
        block_decoder_.init_block(range_coder_, reserved, channels, bitspersample, block_values_);
        if (is_float) float_block_.init(std::move(deta));
    } catch (const OFR_StreamError&) {
        block_error_ = true;
    } catch (const std::bad_alloc&) {
        block_error_ = true;
    } catch (const std::length_error&) {
        block_error_ = true;
    }
    if (block_error_) {
        has_recoverable_errors = true;
        block_values_ = (uint32_t)std::min<uint64_t>(block_values_, total_samples - position);
        block_values_ -= block_values_ % channels;
    }
    need_new_block_ = block_values_ == 0;
}

uint32_t OFR_DecoderEngine::read(int32_t* dest, uint32_t values, bool keep_partial) {
    OFR_ScopedFpEnv fp_env;
    values -= values % channels;
    uint32_t done = 0;
    try {
        while (done < values && position < total_samples) {
            if (need_new_block_) {
                next_block();
                if (need_new_block_) continue;
            }

            uint32_t n = std::min(block_values_ - block_pos_, values - done);
            if (!block_error_) {
                try {
                    block_decoder_.decode_block(dest + done, n, range_coder_);
                    if (is_float) float_block_.reconstruct(dest + done, n);
                } catch (const OFR_StreamError&) {
                    block_error_ = true;
                    has_recoverable_errors = true;
                }
            }
            if (block_error_) std::memset(dest + done, 0, (size_t)n * sizeof(int32_t));

            done += n;
            block_pos_ += n;
            position += n;
            if (block_pos_ == block_values_) need_new_block_ = true;
        }
    } catch (const OFR_StreamError&) {
        if (done == 0 || !keep_partial) throw;
    }
    return done;
}

// walks the block headers from the closest indexed block, without decoding anything
bool OFR_DecoderEngine::hop_to_block(uint64_t target) {
    uint64_t offset = first_block_pos_, value_pos = 0;
    auto it = std::upper_bound(index_.begin(), index_.end(), target,
                               [](uint64_t t, const IndexEntry& e) { return t < e.value_pos; });
    if (it != index_.begin()) {
        --it;
        offset = it->offset;
        value_pos = it->value_pos;
    }

    while (true) {
        if (!bs_.seek(offset) || bs_.read_u32() != OFR_TAG_COMP) return false;
        uint32_t comp_size = bs_.read_u32();
        if (comp_size < 13) return false;
        bs_.read_u32();   // crc32
        uint32_t values = bs_.read_u32();
        remember_block(offset, value_pos, values);
        if (target < value_pos + values) break;

        uint64_t next = offset + 8 + comp_size;
        if (is_float) {
            if (!bs_.seek(next) || bs_.read_u32() != OFR_TAG_DETA) return false;
            next += 8 + (uint64_t)bs_.read_u32();
        }
        offset = next;
        value_pos += values;
    }

    if (!bs_.seek(offset)) return false;
    next_block_pos_ = offset;
    position = value_pos;
    need_new_block_ = true;
    return true;
}

// blocks are independent, so a seek is a hop to the right block plus decoding up to the target
bool OFR_DecoderEngine::seek(uint64_t target) {
    if (target == position) return true;
    if (target >= total_samples || !bs_.seekable()) return false;

    const bool in_current_block = !need_new_block_ && target > position &&
                                  target < position - block_pos_ + block_values_;
    if (!in_current_block) {
        try {
            if (!hop_to_block(target)) return false;
        } catch (const OFR_StreamError&) {
            return false;
        }
    }

    scratch_.resize(1u << 16);
    while (position < target) {
        uint32_t n = (uint32_t)std::min<uint64_t>(target - position, scratch_.size());
        if (read(scratch_.data(), n) == 0) return false;
    }
    return true;
}

void OFR_DecoderEngine::read_tail() {
    uint32_t magic = bs_.peek_tag();
    if (magic != OFR_TAG_TAIL) {
        has_recoverable_errors = true;
        if (!bs_.seek(next_block_pos_) || bs_.read_u32() != OFR_TAG_TAIL) throw OFR_StreamError("no TAIL block");
    }
    uint32_t tail_size = bs_.read_u32();
    tail.clear();
    if (bs_.read_append(tail, tail_size) != tail_size) throw OFR_StreamError("unexpected end of stream");
}

const char* ofr_method_name(uint32_t method) {
    static const char* const names[] = {"fast", "normal", "high", "extra", "best", "ultra", "insane", "highnew",
                                        "extranew", "bestnew", "ultranew", "extrafast", "turbonew", "fastnew",
                                        "normalnew"};
    return method < sizeof(names) / sizeof(names[0]) ? names[method] : "unknown";
}

const char* ofr_speedup_name(uint32_t speedup) {
    static const char* const names[] = {"1x", "2x", "4x"};
    return speedup < sizeof(names) / sizeof(names[0]) ? names[speedup] : "unknown";
}

const char* ofr_sample_type_name(uint32_t sample_type) {
    static const char* const names[] = {"UINT8", "SINT8", "UINT16", "SINT16", "UINT24", "SINT24", "UINT32", "SINT32",
                                        "FLOAT32_1", "FLOAT32_16", "FLOAT32_24"};
    return sample_type < sizeof(names) / sizeof(names[0]) ? names[sample_type] : "unknown";
}

const char* ofr_channel_config_name(uint32_t channel_config) {
    static const char* const names[] = {"MONO", "STEREO_LR"};
    return channel_config < sizeof(names) / sizeof(names[0]) ? names[channel_config] : "unknown";
}
