#pragma once
#include "spu_helpers.h"
#include <immintrin.h>

/* Only the Multistream audio worker installs round-toward-zero, DAZ and FTZ.
 * SSE/FMA can then implement ordinary SPU lanes without the SDK's per-call
 * canonicalization and AVX-512 marshalling. Extended/overflow-risk operands
 * still use the SDK, as do unsupported CPUs and native validation mode.
 * Compile these translations with contraction disabled: FA after FM is not FMA.
 */
extern _Thread_local int haze_audio_scope;
/* Save both thread-local state and MXCSR: nested jobs may temporarily leave
 * the audio image, and spu_run_with_halt returns after a guest longjmp. */
typedef struct haze_audio_float_state {
    unsigned mxcsr;
    unsigned host_mxcsr;
    int active;
} haze_audio_float_state;
haze_audio_float_state haze_audio_enter(int audio_image);
void haze_audio_leave(haze_audio_float_state saved);
extern _Thread_local unsigned haze_audio_host_mxcsr;
__m128 __vectorcall haze_audio_madd_ps(__m128 a, __m128 b, __m128 c);
#define HAZE_AUDIO_INLINE static inline __attribute__((always_inline))
/* SPU flushes either signed zero to +0. FTZ handles nonzero subnormals. */
HAZE_AUDIO_INLINE __m128 haze_zero_sign(__m128 x) {
    return _mm_andnot_ps(_mm_cmpeq_ps(x, _mm_setzero_ps()), x);
}
HAZE_AUDIO_INLINE u128 haze_audio_add(u128 a, u128 b) {
    /* Exponents through 253 cannot overflow the ordinary host range. */
    __m128i em = _mm_set1_epi32(0x7f800000);
    __m128i largest = _mm_max_epu32(_mm_and_si128(_mm_loadu_si128((const __m128i*)&a), em),
                                    _mm_and_si128(_mm_loadu_si128((const __m128i*)&b), em));
    if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(largest, _mm_set1_epi32(0x7e800000)))))
        return spu_float_native_add(a, b);
    u128 r;
    _mm_storeu_ps(r._f32, haze_zero_sign(_mm_add_ps(_mm_loadu_ps(a._f32), _mm_loadu_ps(b._f32))));
    return r;
}
HAZE_AUDIO_INLINE u128 haze_audio_mul(u128 a, u128 b) {
    /* A conservative exponent-sum bound leaves room for significand carry. */
    __m128i em = _mm_set1_epi32(255);
    __m128i ea = _mm_and_si128(_mm_srli_epi32(_mm_loadu_si128((const __m128i*)&a), 23), em);
    __m128i eb = _mm_and_si128(_mm_srli_epi32(_mm_loadu_si128((const __m128i*)&b), 23), em);
    __m128i unsafe = _mm_or_si128(_mm_cmpeq_epi32(_mm_max_epu32(ea, eb), em),
                                  _mm_cmpgt_epi32(_mm_add_epi32(ea, eb), _mm_set1_epi32(379)));
    if (_mm_movemask_ps(_mm_castsi128_ps(unsafe)))
        return spu_float_native_mul(a, b);
    u128 r;
    _mm_storeu_ps(r._f32, haze_zero_sign(_mm_mul_ps(_mm_loadu_ps(a._f32), _mm_loadu_ps(b._f32))));
    return r;
}
HAZE_AUDIO_INLINE u128 haze_audio_fa(u128 a, u128 b) {
    if (spu_float_native_active() && haze_audio_scope)
        return haze_audio_add(a, b);
    return spu_fa(a, b);
}
HAZE_AUDIO_INLINE u128 haze_audio_fs(u128 a, u128 b) {
    if (spu_float_native_active() && haze_audio_scope) {
        for (int i = 0; i < 4; ++i)
            b._u32[i] ^= 0x80000000u;
        return haze_audio_add(a, b);
    }
    return spu_fs(a, b);
}
HAZE_AUDIO_INLINE u128 haze_audio_fm(u128 a, u128 b) {
    if (spu_float_native_active() && haze_audio_scope)
        return haze_audio_mul(a, b);
    return spu_fm(a, b);
}
HAZE_AUDIO_INLINE u128 haze_audio_fma(u128 a, u128 b, u128 c) {
    if (spu_float_native_active() && haze_audio_scope) {
        u128 r;
        _mm_storeu_ps(r._f32, haze_audio_madd_ps(_mm_loadu_ps(a._f32), _mm_loadu_ps(b._f32),
                                                 _mm_loadu_ps(c._f32)));
        return r;
    }
    return spu_fma(a, b, c);
}
HAZE_AUDIO_INLINE u128 haze_audio_fms(u128 a, u128 b, u128 c) {
    if (spu_float_native_active() && haze_audio_scope) {
        for (int i = 0; i < 4; ++i)
            c._u32[i] ^= 0x80000000u;
        return haze_audio_fma(a, b, c);
    }
    return spu_fms(a, b, c);
}
HAZE_AUDIO_INLINE u128 haze_audio_fnms(u128 a, u128 b, u128 c) {
    if (spu_float_native_active() && haze_audio_scope) {
        for (int i = 0; i < 4; ++i)
            a._u32[i] ^= 0x80000000u;
        return haze_audio_fma(a, b, c);
    }
    return spu_fnms(a, b, c);
}
#undef HAZE_AUDIO_INLINE

/* Double arithmetic/conversions retain the SDK host environment. */
u128 haze_audio_dfa(u128 a, u128 b);
u128 haze_audio_dfs(u128 a, u128 b);
u128 haze_audio_dfm(u128 a, u128 b);
u128 haze_audio_dfma(u128 a, u128 b, u128 c);
u128 haze_audio_dfms(u128 a, u128 b, u128 c);
u128 haze_audio_dfnms(u128 a, u128 b, u128 c);
u128 haze_audio_dfnma(u128 a, u128 b, u128 c);
u128 haze_audio_dfceq(u128 a, u128 b);
u128 haze_audio_dfcmeq(u128 a, u128 b);
u128 haze_audio_dfcgt(u128 a, u128 b);
u128 haze_audio_dfcmgt(u128 a, u128 b);
u128 haze_audio_fesd(u128 a);
u128 haze_audio_frds(u128 a);

#ifndef HAZE_AUDIO_FLOAT_NO_REMAP
#define spu_fa haze_audio_fa
#define spu_fs haze_audio_fs
#define spu_fm haze_audio_fm
#define spu_fma haze_audio_fma
#define spu_fms haze_audio_fms
#define spu_fnms haze_audio_fnms
#define spu_dfa haze_audio_dfa
#define spu_dfs haze_audio_dfs
#define spu_dfm haze_audio_dfm
#define spu_dfma haze_audio_dfma
#define spu_dfms haze_audio_dfms
#define spu_dfnms haze_audio_dfnms
#define spu_dfnma haze_audio_dfnma
#define spu_dfceq haze_audio_dfceq
#define spu_dfcmeq haze_audio_dfcmeq
#define spu_dfcgt haze_audio_dfcgt
#define spu_dfcmgt haze_audio_dfcmgt
#define spu_fesd haze_audio_fesd
#define spu_frds haze_audio_frds
#endif
