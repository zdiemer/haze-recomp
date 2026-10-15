#define HAZE_AUDIO_FLOAT_NO_REMAP
#include "haze_audio_spu_float.h"
#include "spu_context.h"
#include <stdlib.h>

_Thread_local int haze_audio_scope;
_Thread_local unsigned haze_audio_host_mxcsr;

haze_audio_float_state haze_audio_enter(int audio_image) {
    haze_audio_float_state saved = {_mm_getcsr(), haze_audio_host_mxcsr, haze_audio_scope};
    const unsigned host = saved.active ? saved.host_mxcsr : saved.mxcsr;
    int info[4];
    __cpuidex(info, 1, 0);
    const char* validate = getenv("SPU_FLOAT_NATIVE_VALIDATE");
    haze_audio_host_mxcsr = host;
    haze_audio_scope = audio_image && spu_float_native_active() && (info[2] & (1 << 12)) &&
                       !(validate && *validate == '1');
    /* RTZ, DAZ, FTZ and masked exceptions; preserve the caller's status flags
     * on entry and restore its complete control/status register on exit. */
    _mm_setcsr(haze_audio_scope ? host | 0xffc0u : host);
    return saved;
}

void haze_audio_leave(haze_audio_float_state saved) {
    haze_audio_scope = saved.active;
    haze_audio_host_mxcsr = saved.host_mxcsr;
    _mm_setcsr(saved.mxcsr);
}

/* The renamed SDK function owns the setjmp/longjmp landing pad. Keeping this
 * wrapper outside it restores MXCSR on normal return and on guest halt.
 * Image 27 is spu_0026_at_00DAFD00, Haze's Multistream worker; its dynamically
 * dispatched audio PICs and ATRAC decoder execute on this same host thread. */
int haze_sdk_spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx) {
    if (!haze_audio_scope && ctx->image_id != 27)
        return haze_sdk_spu_run_with_halt(entry, ctx);
    haze_audio_float_state saved = haze_audio_enter(ctx->image_id == 27);
    int result = haze_sdk_spu_run_with_halt(entry, ctx);
    haze_audio_leave(saved);
    return result;
}

__attribute__((target("fma"))) __m128 __vectorcall haze_audio_madd_ps(__m128 a, __m128 b,
                                                                      __m128 c) {
    const __m128i em = _mm_set1_epi32(255);
    __m128i ea = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(a), 23), em);
    __m128i eb = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(b), 23), em);
    __m128i ec = _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(c), 23), em);
    __m128i unsafe = _mm_or_si128(_mm_cmpeq_epi32(_mm_max_epu32(ea, eb), em),
                                  _mm_cmpgt_epi32(ec, _mm_set1_epi32(253)));
    unsafe = _mm_or_si128(unsafe, _mm_cmpgt_epi32(_mm_add_epi32(ea, eb), _mm_set1_epi32(379)));
    if (_mm_movemask_ps(_mm_castsi128_ps(unsafe)))
        return spu_float_native_madd_ps(a, b, c);
    return haze_zero_sign(_mm_fmadd_ps(a, b, c));
}
