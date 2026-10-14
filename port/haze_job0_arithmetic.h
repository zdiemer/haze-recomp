#ifndef HAZE_JOB0_ARITHMETIC_H
#define HAZE_JOB0_ARITHMETIC_H

/*
 * Experimental AVX-512 arithmetic for the private native SPU job-0 kernel.
 * Each u128 contains four raw SPU single-precision lanes in _u32[0..3].
 * Safe lanes use AVX-512 round-toward-zero with exceptions suppressed and
 * canonical/flush handling identical to spu_float_native.c. Risky lanes use
 * the SDK's integer SPU arithmetic helpers, preserving their exact behavior.
 *
 * The caller must select this path once, before entering a loop, only when
 * spu_float_native_available() and the chosen native-mode policy permit it.
 * These functions intentionally do not inspect runtime mode, validate output,
 * or perform feature detection per operation. They require AVX512F.
 */

#include "spu_helpers.h"
#include <immintrin.h>

#define HAZE_JOB0_AVX512 __attribute__((target("avx512f")))
#define HAZE_JOB0_INLINE static inline __attribute__((always_inline))
#define HAZE_JOB0_ROUND (_MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)

HAZE_JOB0_INLINE __m128i haze_job0_canonical(__m128i x) {
    const __m128i exponent = _mm_and_si128(x, _mm_set1_epi32(0x7f800000));
    return _mm_andnot_si128(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()), x);
}

HAZE_JOB0_INLINE __m128i haze_job0_flush(__m128i x) {
    return haze_job0_canonical(x);
}

HAZE_JOB0_AVX512 HAZE_JOB0_INLINE u128 haze_job0_add(u128 a, u128 b) {
    const __m128i aa = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&a));
    const __m128i bb = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&b));
    const __m128i exponent_mask = _mm_set1_epi32(0x7f800000);
    const __m128i limit = _mm_set1_epi32(0x7e800000);
    const int slow = _mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(
        _mm_cmpgt_epi32(_mm_and_si128(aa, exponent_mask), limit),
        _mm_cmpgt_epi32(_mm_and_si128(bb, exponent_mask), limit))));

    const __m512 result = _mm512_maskz_add_round_ps(
        15, _mm512_castps128_ps512(_mm_castsi128_ps(aa)),
        _mm512_castps128_ps512(_mm_castsi128_ps(bb)), HAZE_JOB0_ROUND);
    u128 r;
    _mm_storeu_si128((__m128i *)&r,
        haze_job0_flush(_mm_castps_si128(_mm512_castps512_ps128(result))));
    for (int i = 0; i < 4; ++i)
        if (slow & (1 << i)) r._u32[i] = spu_float_add(a._u32[i], b._u32[i]);
    return r;
}

HAZE_JOB0_AVX512 HAZE_JOB0_INLINE u128 haze_job0_sub(u128 a, u128 b) {
    for (int i = 0; i < 4; ++i) b._u32[i] ^= 0x80000000u;
    return haze_job0_add(a, b);
}

HAZE_JOB0_AVX512 HAZE_JOB0_INLINE u128 haze_job0_mul(u128 a, u128 b) {
    const __m128i aa = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&a));
    const __m128i bb = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&b));
    const __m128i exponent_mask = _mm_set1_epi32(0xff);
    const __m128i ea = _mm_and_si128(_mm_srli_epi32(aa, 23), exponent_mask);
    const __m128i eb = _mm_and_si128(_mm_srli_epi32(bb, 23), exponent_mask);
    __m128i unsafe = _mm_or_si128(_mm_cmpeq_epi32(ea, exponent_mask),
                                   _mm_cmpeq_epi32(eb, exponent_mask));
    unsafe = _mm_or_si128(unsafe,
        _mm_cmpgt_epi32(_mm_add_epi32(ea, eb), _mm_set1_epi32(379)));
    const int slow = _mm_movemask_ps(_mm_castsi128_ps(unsafe));

    const __m512 result = _mm512_maskz_mul_round_ps(
        15, _mm512_castps128_ps512(_mm_castsi128_ps(aa)),
        _mm512_castps128_ps512(_mm_castsi128_ps(bb)), HAZE_JOB0_ROUND);
    u128 r;
    _mm_storeu_si128((__m128i *)&r,
        haze_job0_flush(_mm_castps_si128(_mm512_castps512_ps128(result))));
    for (int i = 0; i < 4; ++i)
        if (slow & (1 << i)) r._u32[i] = spu_float_mul(a._u32[i], b._u32[i]);
    return r;
}

HAZE_JOB0_AVX512 HAZE_JOB0_INLINE u128 haze_job0_madd(u128 a, u128 b, u128 c) {
    const __m128i aa = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&a));
    const __m128i bb = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&b));
    const __m128i cc = haze_job0_canonical(_mm_loadu_si128((const __m128i *)&c));
    const __m128i exponent_mask = _mm_set1_epi32(0xff);
    const __m128i ea = _mm_and_si128(_mm_srli_epi32(aa, 23), exponent_mask);
    const __m128i eb = _mm_and_si128(_mm_srli_epi32(bb, 23), exponent_mask);
    const __m128i ec = _mm_and_si128(_mm_srli_epi32(cc, 23), exponent_mask);
    __m128i unsafe = _mm_or_si128(_mm_cmpeq_epi32(ea, exponent_mask),
                                   _mm_cmpeq_epi32(eb, exponent_mask));
    unsafe = _mm_or_si128(unsafe, _mm_cmpgt_epi32(ec, _mm_set1_epi32(253)));
    unsafe = _mm_or_si128(unsafe,
        _mm_cmpgt_epi32(_mm_add_epi32(ea, eb), _mm_set1_epi32(379)));
    const int slow = _mm_movemask_ps(_mm_castsi128_ps(unsafe));

    const __m512 result = _mm512_maskz_fmadd_round_ps(
        15, _mm512_castps128_ps512(_mm_castsi128_ps(aa)),
        _mm512_castps128_ps512(_mm_castsi128_ps(bb)),
        _mm512_castps128_ps512(_mm_castsi128_ps(cc)), HAZE_JOB0_ROUND);
    u128 r;
    _mm_storeu_si128((__m128i *)&r,
        haze_job0_flush(_mm_castps_si128(_mm512_castps512_ps128(result))));
    for (int i = 0; i < 4; ++i)
        if (slow & (1 << i)) r._u32[i] = spu_float_madd(a._u32[i], b._u32[i], c._u32[i]);
    return r;
}

/* SPU fnms is c - a*b: negate the multiplicand by flipping its sign bit,
 * then use the same fused operation and risk boundary as haze_job0_madd. */
HAZE_JOB0_AVX512 HAZE_JOB0_INLINE u128 haze_job0_nms(u128 a, u128 b, u128 c) {
    for (int i = 0; i < 4; ++i) a._u32[i] ^= 0x80000000u;
    return haze_job0_madd(a, b, c);
}

#undef HAZE_JOB0_ROUND
#undef HAZE_JOB0_INLINE
#undef HAZE_JOB0_AVX512

#endif /* HAZE_JOB0_ARITHMETIC_H */
