#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>

void* handle;

extern "C" {
    void FUN_00006f50(void* param_1, uint32_t param_2, uint32_t param_3, int param_4, uint64_t param_5) {
        printf("FUN_00006f50 called! param_1=%p\n", param_1);
        uint64_t vtable = *(uint64_t*)param_1;
        printf("vtable = %llx\n", (unsigned long long)vtable);
        
        uint64_t slide = 0;
        // The vtable points inside the dylib.
        printf("vtable[0] = %llx\n", ((uint64_t*)vtable)[0]);
        printf("vtable[1] = %llx\n", ((uint64_t*)vtable)[1]);
        printf("vtable[2] = %llx\n", ((uint64_t*)vtable)[2]);
        printf("vtable[3] = %llx\n", ((uint64_t*)vtable)[3]);
    }
}
