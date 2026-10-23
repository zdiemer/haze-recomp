#ifndef HAZE_JOB13_NATIVE_MATH_H
#define HAZE_JOB13_NATIVE_MATH_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>

static inline double haze_job13_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* The translated double helpers also evaluate the unused vector lane. Its
 * input is old stack data, not the caller's second input lane. Reproduce its
 * arithmetic for FP status (including denormal/NaN cases), without emulating
 * integer division or scratch registers. Normal preferred divisions return
 * an upper +0 lane. The wrapper must validate that packing-table contract.
 * Volatile intermediates keep these status-only operations observable. */
static inline void haze_job13_native_upper_sqrt_effects(uint64_t input_bits, uint64_t upper_bits) {
#if defined(__clang__)
#pragma clang fp exceptions(strict)
#endif
    volatile __m128d a = _mm_set_sd(haze_job13_double(upper_bits));
    __m128d z = _mm_setzero_pd();
    if ((input_bits >> 52) & 1u) a = _mm_add_sd(a, a);
    volatile __m128d q = _mm_mul_sd(a, z);
    q = _mm_add_sd(q, z);
    q = _mm_mul_sd(a, q);
    q = _mm_add_sd(q, z);
    q = _mm_add_sd(q, z);
    q = _mm_mul_sd(q, z);
    q = _mm_add_sd(q, z);
    q = _mm_add_sd(q, z);
    q = _mm_mul_sd(q, z);
}

/* Typed positive-normal path of the effects library's square-root algorithm.
 * Preserve its polynomial and three Newton divisions; do not substitute sqrt.
 * Explicit scalar instructions prevent reassociation or multiply-add fusion.
 * This component does not implement the guest calling convention or publish
 * any object state. Unsupported modes and inputs leave the output unchanged. */
static inline bool haze_job13_positive_sqrt(double input, double* output) {
    uint64_t bits;
    memcpy(&bits, &input, sizeof bits);
    unsigned exponent = (unsigned)((bits >> 52) & 2047u);
    unsigned csr = _mm_getcsr();
    if ((bits >> 63) || !exponent || exponent == 2047u ||
        (csr & 0x6000u) || (csr & 0x1f80u) != 0x1f80u)
        return false;
    int power = (int)exponent - 1022;
    bits = (bits & UINT64_C(0x000fffffffffffff)) | UINT64_C(0x3fe0000000000000);
    if (power % 2) {
        bits += UINT64_C(0x0010000000000000);
        --power;
    }
    __m128d m = _mm_set_sd(haze_job13_double(bits));
    __m128d c0 = _mm_set_sd(haze_job13_double(UINT64_C(0xbfb98a86d71f3626)));
    __m128d c1 = _mm_set_sd(haze_job13_double(UINT64_C(0x3fe6bb2fec56d5d0)));
    __m128d c2 = _mm_set_sd(haze_job13_double(UINT64_C(0x3fd8be0ded288ce7)));
    __m128d y = _mm_add_sd(_mm_mul_sd(m, _mm_add_sd(_mm_mul_sd(m, c0), c1)), c2);
    __m128d t = _mm_add_sd(y, _mm_div_sd(m, y));
    y = _mm_add_sd(_mm_mul_sd(t, _mm_set_sd(0.25)), _mm_div_sd(m, t));
    y = _mm_mul_sd(_mm_add_sd(y, _mm_div_sd(m, y)), _mm_set_sd(0.5));
    double result = _mm_cvtsd_f64(y);
    memcpy(&bits, &result, sizeof bits);
    bits += (uint64_t)(int64_t)(power / 2) << 52;
    *output = haze_job13_double(bits);
    return true;
}

#endif
