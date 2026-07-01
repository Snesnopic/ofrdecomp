// minimal reference decoder wrapper around libOptimFROG, used by the
// conformance suite (tests/conformance/run.sh) to produce ground-truth raw
// PCM for direction A/B comparisons. Build:
//   clang -arch x86_64 -I include tests/conformance/ref_gen.c \
//       libOptimFROG.0.dylib -o /tmp/ref_gen
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "OptimFROG.h"

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: ref_gen in.ofr out.raw\n"); return 2; }
    void* d = OptimFROG_createInstance();
    if (!d) { fprintf(stderr, "no instance\n"); return 3; }
    if (!OptimFROG_open(d, argv[1], 0)) { fprintf(stderr, "open fail\n"); return 4; }
    OptimFROG_Info info; memset(&info, 0, sizeof(info));
    OptimFROG_getInfo(d, &info);
    int ch = info.channels, bps = info.bitspersample, bpv = bps / 8;
    fprintf(stderr, "ch=%d bps=%d sr=%d\n", ch, bps, info.samplerate);
    FILE* f = fopen(argv[2], "wb");
    if (!f) { return 5; }
    long total = (long)info.noPoints; // frames
    const int CH = 65536;
    unsigned char* buf = malloc((size_t)CH * ch * bpv);
    long done = 0;
    while (done < total) {
        int want = (total - done < CH) ? (int)(total - done) : CH;
        int got = OptimFROG_read(d, buf, want, 0);
        if (got <= 0) break;
        fwrite(buf, (size_t)got * ch * bpv, 1, f);
        done += got;
    }
    fprintf(stderr, "decoded %ld frames\n", done);
    fprintf(stderr, "recoverableErrors=%d\n", (int)OptimFROG_recoverableErrors(d));
    fclose(f);
    OptimFROG_close(d);
    OptimFROG_destroyInstance(d);
    return 0;
}
