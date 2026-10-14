#pragma once
#include "spu_helpers.h"
#include "spu_float_native.h"
#ifdef __cplusplus
extern "C" {
#endif
int haze_job0_kernel_enabled(void);
void haze_job0_kernel_note(unsigned pc);
#ifdef __cplusplus
}
#endif

static inline int haze_job0_kernel_accept(const spu_context* ctx,unsigned pc)
{
#if defined(SPU_FLOAT_NATIVE_HOST)
    // The private job ABI owns LS; no observer may inspect intermediate GPRs.
    // Unsupported CPUs/modes and every diagnostic observer retain the lift.
    return ctx->image_id==30 && !ctx->policy_mode && ctx->pc==pc &&
        ctx->ls==ctx->ls_store && !g_spu_trampoline_fn &&
        !g_spu_ls_watch_n && !g_spu_ls_probe && !g_spu_smc_watch &&
        spu_float_native_active() && haze_job0_kernel_enabled();
#else
    (void)ctx;(void)pc;return 0;
#endif
}
