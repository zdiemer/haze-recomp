#pragma once
#include "spu_context.h"
#include "haze_job13_native_divide.h"
#include "haze_job13_native_math.h"

#ifdef __cplusplus
extern "C" {
#endif
void haze_job13_native_note(unsigned operation);
#ifdef __cplusplus
}
#endif

enum { HAZE_JOB13_NATIVE_DIVIDE = 0, HAZE_JOB13_NATIVE_SQRT = 1 };

/* Image 32 (effects batch). Native operations replace audited arithmetic only;
 * every unsupported state keeps the original translation. Debug watches and
 * policy/raw-LS modes observe individual LS accesses, so they fall back too. */
static inline int haze_job13_native_ready(const spu_context* ctx)
{
    return ctx->image_id == 32 && ctx->ls == ctx->ls_store && !ctx->policy_mode &&
           g_spu_ls_watch_n == 0 && g_spu_ls_probe == 0 && g_spu_smc_watch == 0;
}
