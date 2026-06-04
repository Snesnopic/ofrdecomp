#include "../include/optimfrog_decoder.h"

OFR_DecoderEngine::~OFR_DecoderEngine() {}
bool OFR_DecoderEngine::open() { return false; }
bool OFR_DecoderEngine::read(void* dest, uInt32_t count) { return false; }
bool OFR_DecoderEngine::seek(sInt64_t sample_pos) { return false; }
bool OFR_DecoderEngine::readTail() { return false; }
void OFR_DecoderEngine::close() {}
