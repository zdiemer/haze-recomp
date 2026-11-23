#ifndef HAZE_JOB0_SIMD_H
#define HAZE_JOB0_SIMD_H

/*
 * Host SIMD helpers for build-generated job-0 loop kernels (#66).
 *
 * A generated kernel keeps every SPU register in an __m128i (_u32[i] = SPU
 * word i) and runs one loop iteration with plain host arithmetic under
 * HAZE_JOB0_SIMD_MXCSR: exceptions masked, round toward zero, flush to zero,
 * denormal inputs not treated as zero. SPU single precision matches that
 * arithmetic whenever no operand is an SPU exponent-255 value (a host NaN or
 * infinity) and the iteration raises no invalid, denormal, divide, overflow
 * or underflow flag; zero results only differ in sign and are canonicalized
 * where their bits are observed. An iteration that fails either check is
 * repeated by the original translation before it stores anything.
 *
 * The caller must already hold haze_job0_kernel_accept() (AVX-512 OS state,
 * no LS observers) and haze_job0_simd_supported().
 */

#include "spu_helpers.h"
#include <immintrin.h>
#include <intrin.h>

#define HAZE_JOB0_SIMD_TARGET \
    __attribute__((target("avx512f,avx512vl,avx512bw,avx512dq,avx512vbmi,fma")))
#define HAZE_JOB0_SIMD_INLINE static inline __attribute__((always_inline))
#define HAZE_JOB0_SIMD_MXCSR 0xFF80u
#define HAZE_JOB0_SIMD_FLAGS 0x1Fu
/* fpclass: QNaN, +Inf, -Inf, SNaN. Exponent 255 is finite on the SPU. */
#define HAZE_JOB0_SIMD_SPECIAL 0x99

/* AVX-512 VL/BW/DQ/VBMI and FMA. OS vector state is checked by
 * spu_float_native_active() in haze_job0_kernel_accept(). */
static inline int haze_job0_simd_supported(void)
{
    static int cached = -1;
    if (cached < 0) {
        int leaf1[4], leaf7[4];
        __cpuid(leaf1, 1);
        __cpuidex(leaf7, 7, 0);
        const unsigned ebx = (unsigned)leaf7[1], ecx = (unsigned)leaf7[2];
        const unsigned need = (1u << 16) | (1u << 17) | (1u << 30) | (1u << 31);
        cached = ((unsigned)leaf1[2] & (1u << 12)) && (ebx & need) == need && (ecx & 2u);
    }
    return cached;
}

HAZE_JOB0_SIMD_INLINE __m128i haze_job0_simd_v(u128 value)
{
    __m128i r; memcpy(&r, &value, 16); return r;
}

HAZE_JOB0_SIMD_INLINE u128 haze_job0_simd_u(__m128i value)
{
    u128 r; memcpy(&r, &value, 16); return r;
}

HAZE_JOB0_SIMD_INLINE uint32_t haze_job0_simd_word(__m128i value)
{
    return (uint32_t)_mm_cvtsi128_si32(value);
}

/* Local store is big-endian per word; registers hold host-order words. */
HAZE_JOB0_SIMD_TARGET HAZE_JOB0_SIMD_INLINE __m128i haze_job0_simd_ld(const uint8_t* ls, uint32_t lsa)
{
    const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (lsa & SPU_LS_MASK & ~0xFu))), swap);
}

HAZE_JOB0_SIMD_TARGET HAZE_JOB0_SIMD_INLINE void haze_job0_simd_st(uint8_t* ls, uint32_t lsa, __m128i value)
{
    const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    _mm_storeu_si128((__m128i*)(ls + (lsa & SPU_LS_MASK & ~0xFu)), _mm_shuffle_epi8(value, swap));
}

/* shufb index for a loop-invariant control without special selectors:
 * selector s picks SPU byte s & 31 of {a, b}, host byte (s & 31) ^ 3. */
HAZE_JOB0_SIMD_TARGET HAZE_JOB0_SIMD_INLINE int haze_job0_simd_control(__m128i control, __m128i* index)
{
    *index = _mm_xor_si128(control, _mm_set1_epi8(3));
    return _mm_movemask_epi8(control) == 0;
}

HAZE_JOB0_SIMD_TARGET HAZE_JOB0_SIMD_INLINE __mmask8 haze_job0_simd_special(__m128i value)
{
    return _mm_fpclass_ps_mask(_mm_castsi128_ps(value), HAZE_JOB0_SIMD_SPECIAL);
}

/* SPU arithmetic never produces -0. */
HAZE_JOB0_SIMD_TARGET HAZE_JOB0_SIMD_INLINE __m128i haze_job0_simd_zero(__m128i value)
{
    return _mm_andnot_si128(_mm_cmpeq_epi32(value, _mm_set1_epi32((int)0x80000000u)), value);
}

#endif /* HAZE_JOB0_SIMD_H */
