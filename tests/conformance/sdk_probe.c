/* exercises the C API and prints what every call returns. built once against the official library
   and once against this one, the two transcripts must match (see sdk_diff.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "OptimFROG.h"

static unsigned long hash(const unsigned char* p, size_t n, unsigned long h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ul;
    return h;
}

static void print_info(const char* label, const OptimFROG_Info* in) {
    printf("%s ch=%u rate=%u bps=%u bitrate=%u version=%u method=%s speedup=%s points=%lld orig=%lld comp=%lld ms=%lld type=%s config=%s\n",
           label, in->channels, in->samplerate, in->bitspersample, in->bitrate, in->version,
           in->method ? in->method : "(null)", in->speedup ? in->speedup : "(null)", (long long)in->noPoints,
           (long long)in->originalSize, (long long)in->compressedSize, (long long)in->length_ms,
           in->sampleType ? in->sampleType : "(null)", in->channelConfig ? in->channelConfig : "(null)");
}

static void print_tags(const char* label, const OptimFROG_Tags* t) {
    printf("%s count=%u\n", label, t->keyCount);
    for (uInt32_t i = 0; i < t->keyCount; i++) printf("  [%s]=[%s]\n", t->keys[i], t->values[i]);
}

static int progress_calls;
static double progress_last;
static void progress(void* param, Float64_t pct) {
    (void)param;
    if (progress_calls < 3) printf("  progress %.6f\n", pct);
    progress_calls++;
    progress_last = pct;
}

static void read_all(void* d, const char* label, int max16) {
    OptimFROG_Info in;
    OptimFROG_getInfo(d, &in);
    size_t frame = (size_t)in.channels * (max16 && in.bitspersample > 16 ? 2 : in.bitspersample / 8);
    unsigned char* buf = malloc(100000 * frame);
    const uInt32_t sizes[] = {1000, 3, 77777, 1, 4096};
    unsigned long h = 14695981039346656037ul;
    long long total = 0;
    for (int i = 0;; i++) {
        sInt32_t got = OptimFROG_read(d, buf, sizes[i % 5], (condition_t)max16);
        if (got <= 0) { printf("%s last read=%d after %lld points pos=%lld\n", label, got, total, (long long)OptimFROG_getPos(d)); break; }
        h = hash(buf, (size_t)got * frame, h);
        total += got;
    }
    printf("%s hash=%016lx recoverable=%d\n", label, h, OptimFROG_recoverableErrors(d));
    free(buf);
}

static void probe_file(const char* path) {
    printf("== %s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    void* d = OptimFROG_createInstance();
    condition_t ok = OptimFROG_open(d, path, C_TRUE);
    printf("open=%d\n", ok);
    if (!ok) { OptimFROG_destroyInstance(d); return; }

    OptimFROG_Info in;
    memset(&in, 0, sizeof(in));
    printf("getInfo=%d seekable=%d pos=%lld\n", OptimFROG_getInfo(d, &in), OptimFROG_seekable(d), (long long)OptimFROG_getPos(d));
    print_info("info", &in);

    OptimFROG_Tags tags;
    memset(&tags, 0, sizeof(tags));
    printf("getTags=%d\n", OptimFROG_getTags(d, &tags));
    print_tags("tags", &tags);
    OptimFROG_freeTags(&tags);

    char small[4], big[4096];
    memset(small, 0, sizeof(small));
    printf("readHead(0)=%d readHead(4)=%d", OptimFROG_readHead(d, NULL, 0), OptimFROG_readHead(d, small, sizeof(small)));
    sInt32_t hn = OptimFROG_readHead(d, big, sizeof(big));
    printf(" readHead(4096)=%d hash=%016lx\n", hn, hn > 0 ? hash((unsigned char*)big, (size_t)hn, 1) : 0);
    printf("readTail before end=%d\n", OptimFROG_readTail(d, big, sizeof(big)));

    read_all(d, "read", 0);
    printf("read at end=%d\n", OptimFROG_read(d, big, 10, C_FALSE));
    sInt32_t tn = OptimFROG_readTail(d, big, sizeof(big));
    printf("readTail(0)=%d readTail(4096)=%d hash=%016lx\n", OptimFROG_readTail(d, NULL, 0), tn, tn > 0 ? hash((unsigned char*)big, (size_t)tn, 1) : 0);

    long long n = in.noPoints;
    long long targets[] = {n, n + 5, -1, n / 2, 0, n - 1, n / 3};
    for (int i = 0; i < 7; i++) {
        condition_t s = OptimFROG_seekPoint(d, targets[i]);
        printf("seekPoint(%lld)=%d pos=%lld", targets[i], s, (long long)OptimFROG_getPos(d));
        unsigned char buf[4096];
        sInt32_t got = OptimFROG_read(d, buf, 100, C_FALSE);
        printf(" read=%d", got);
        if (got > 0) printf(" hash=%016lx", hash(buf, (size_t)got * in.channels * (in.bitspersample / 8), 1));
        printf(" pos=%lld\n", (long long)OptimFROG_getPos(d));
    }
    printf("seekTime(250)=%d pos=%lld\n", OptimFROG_seekTime(d, 250), (long long)OptimFROG_getPos(d));

    if (in.bitspersample > 16 && OptimFROG_seekPoint(d, 0)) read_all(d, "read16", 1);

    printf("close=%d close again=%d\n", OptimFROG_close(d), OptimFROG_close(d));
    OptimFROG_destroyInstance(d);

    memset(&in, 0, sizeof(in));
    memset(&tags, 0, sizeof(tags));
    printf("infoFile=%d\n", OptimFROG_infoFile(path, &in, &tags));
    print_info("infoFile", &in);
    print_tags("infoFile tags", &tags);
    OptimFROG_freeTags(&tags);
}

static void probe_decode(const char* path, const char* out) {
    progress_calls = 0;
    progress_last = -1;
    sInt32_t r = OptimFROG_decodeFile(path, out, progress, NULL);
    unsigned long h = 1;
    long size = 0;
    FILE* f = fopen(out, "rb");
    if (f) {
        unsigned char buf[65536];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) { h = hash(buf, got, h); size += (long)got; }
        fclose(f);
        remove(out);
    }
    printf("decodeFile(%s)=%d calls=%d last=%.6f size=%ld hash=%016lx\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
           r, progress_calls, progress_last, size, h);
}

struct mem { unsigned char* data; long size, pos; int closes; };
static condition_t m_close(void* i) { ((struct mem*)i)->closes++; return C_TRUE; }
static sInt32_t m_read(void* i, void* dest, uInt32_t n) {
    struct mem* m = i;
    long left = m->size - m->pos;
    long got = (long)n < left ? (long)n : left;
    memcpy(dest, m->data + m->pos, (size_t)got);
    m->pos += got;
    return (sInt32_t)got;
}
static condition_t m_eof(void* i) { struct mem* m = i; return m->pos >= m->size; }
static condition_t m_no(void* i) { (void)i; return C_FALSE; }
static sInt64_t m_length(void* i) { (void)i; return -1; }
static sInt64_t m_pos(void* i) { return ((struct mem*)i)->pos; }
static condition_t m_seek(void* i, sInt64_t p) { (void)i; (void)p; return C_FALSE; }

static void probe_stream(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    struct mem m = {malloc(64 << 20), 0, 0, 0};
    m.size = (long)fread(m.data, 1, 64 << 20, f);
    fclose(f);
    ReadInterface ri = {m_close, m_read, m_eof, m_no, m_length, m_pos, m_seek};
    void* d = OptimFROG_createInstance();
    condition_t ok = OptimFROG_openExt(d, &ri, &m, C_TRUE);
    printf("== stream %s openExt=%d closes=%d\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, ok, m.closes);
    if (ok) {
        OptimFROG_Info in;
        OptimFROG_getInfo(d, &in);
        print_info("stream info", &in);
        OptimFROG_Tags tags;
        memset(&tags, 0, sizeof(tags));
        printf("stream seekable=%d getTags=%d count=%u seekPoint=%d\n", OptimFROG_seekable(d), OptimFROG_getTags(d, &tags),
               tags.keyCount, OptimFROG_seekPoint(d, 10));
        read_all(d, "stream read", 0);
        printf("stream close=%d closes=%d\n", OptimFROG_close(d), m.closes);
    }
    OptimFROG_destroyInstance(d);
    printf("stream after destroy closes=%d\n", m.closes);
    free(m.data);
}

int main(int argc, char** argv) {
    printf("version=%u\n", OptimFROG_getVersion());
    void* d = OptimFROG_createInstance();
    printf("close unopened=%d open missing=%d\n", OptimFROG_close(d), OptimFROG_open(d, "/nonexistent/x.ofr", C_FALSE));
    OptimFROG_destroyInstance(d);
    OptimFROG_Info in;
    printf("infoFile missing=%d\n", OptimFROG_infoFile("/nonexistent/x.ofr", &in, NULL));
    printf("decodeFile missing=%d\n", OptimFROG_decodeFile("/nonexistent/x.ofr", "/nonexistent/y", NULL, NULL));

    const char* out = argv[1];
    for (int i = 2; i < argc; i++) {
        probe_file(argv[i]);
        probe_decode(argv[i], out);
        probe_stream(argv[i]);
    }
    if (argc > 2) printf("decodeFile unwritable=%d\n", OptimFROG_decodeFile(argv[2], "/nonexistent/dir/y", NULL, NULL));
    return 0;
}
