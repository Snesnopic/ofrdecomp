#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include "OptimFROG.h"

int main() {
    Dl_info info;
    dladdr(OptimFROG_open, &info);
    uint64_t slide = (uint64_t)info.dli_fbase;
    uint8_t** entropy2_vtable = (uint8_t**)(slide + 0x21830);
    
    uint8_t* func = entropy2_vtable[1]; // decode_block
    
    FILE* f = fopen("entropy2_decode.bin", "wb");
    fwrite(func, 1, 4096, f);
    fclose(f);
    
    printf("Dumped entropy2 decode_block to entropy2_decode.bin\n");
    return 0;
}
