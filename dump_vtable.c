#include <stdio.h>
#include <dlfcn.h>
#include <stdint.h>

int main() {
    void* handle = dlopen("./libOptimFROG.dylib", RTLD_NOW);
    if (!handle) {
        printf("dlopen error: %s\n", dlerror());
        return 1;
    }
    // We don't have the symbol for vtable easily.
    // Let's just create an instance and read the vtable!
    void* instance = dlsym(handle, "OptimFROG_createInstance");
    void* (*create)() = instance;
    void* obj = create();
    
    // OFR_DecoderEngine is at obj
    // PostProcessor is at obj + 0x90 + 0xd*8 = obj + 0xf8?
    // In Mac, it might be different!
    // Let's just run it on sine_mono.ofr and hook or search memory?
    return 0;
}
