/* SwitchRecomp: ScarletNX compiles some GPU macros with its LLVM backend (mme_jit.c). This build has no LLVM, so no macro
   has a compiled form and the interpreter (mme.c) runs every one, as ScarletNX itself does with mme_jit=0. */
#include "nx/gpu/gpu.h"
int mme_jit_run(GpuChannel* ch, void* rec, u32 idx, u32 np) {
    (void)ch; (void)rec; (void)idx; (void)np;
    return 0;   /* not compiled: the interpreter runs it */
}
void mme_jit_free(void) {}
void mme_jit_cutoff(GpuChannel* ch) { (void)ch; }
