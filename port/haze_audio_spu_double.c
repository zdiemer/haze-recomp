/* Compile this TU with -ffp-model=strict so the compiler cannot move host
 * operations across the MXCSR switches. Double operations do not have the
 * single-precision SPU flush/rounding rules. The volatile result also forces
 * exact widening conversions to finish before MXCSR is restored: Clang can
 * otherwise sink FESD past the switch even with the strict floating model. */
#define HAZE_AUDIO_FLOAT_NO_REMAP
#include "haze_audio_spu_float.h"

u128 haze_audio_dfa(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfa(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfa(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfs(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfs(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfs(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfm(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfm(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfm(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfma(u128 a, u128 b, u128 c) {
    if (!haze_audio_scope)
        return spu_dfma(a, b, c);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfma(a, b, c);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfms(u128 a, u128 b, u128 c) {
    if (!haze_audio_scope)
        return spu_dfms(a, b, c);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfms(a, b, c);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfnms(u128 a, u128 b, u128 c) {
    if (!haze_audio_scope)
        return spu_dfnms(a, b, c);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfnms(a, b, c);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfnma(u128 a, u128 b, u128 c) {
    if (!haze_audio_scope)
        return spu_dfnma(a, b, c);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfnma(a, b, c);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfceq(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfceq(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfceq(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfcmeq(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfcmeq(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfcmeq(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfcgt(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfcgt(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfcgt(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_dfcmgt(u128 a, u128 b) {
    if (!haze_audio_scope)
        return spu_dfcmgt(a, b);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_dfcmgt(a, b);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_fesd(u128 a) {
    if (!haze_audio_scope)
        return spu_fesd(a);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_fesd(a);
    _mm_setcsr(saved);
    return result;
}

u128 haze_audio_frds(u128 a) {
    if (!haze_audio_scope)
        return spu_frds(a);
    unsigned saved = _mm_getcsr();
    _mm_setcsr(haze_audio_host_mxcsr);
    volatile u128 result = spu_frds(a);
    _mm_setcsr(saved);
    return result;
}
