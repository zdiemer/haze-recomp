#pragma once
#include "spu_helpers.h"

/* Keep the native vector path in the translated DSP loop. The generic SPU
 * helpers contain the large integer fallback too, so /Ob1 can outline them
 * and marshal every u128 operand through memory on the Windows C ABI.
 * Arithmetic and runtime CPU/validation fallback remain the SDK's own. */
#define HAZE_AUDIO_INLINE static inline __attribute__((always_inline))
HAZE_AUDIO_INLINE u128 haze_audio_fa(u128 a, u128 b) {
    if (spu_float_native_active()) return spu_float_native_add(a, b);
    return spu_fa(a, b);
}
HAZE_AUDIO_INLINE u128 haze_audio_fs(u128 a, u128 b) {
    if (spu_float_native_active()) {
        for (unsigned i = 0; i < 4; ++i) b._u32[i] ^= 0x80000000u;
        return spu_float_native_add(a, b);
    }
    return spu_fs(a, b);
}
HAZE_AUDIO_INLINE u128 haze_audio_fm(u128 a, u128 b) {
    if (spu_float_native_active()) return spu_float_native_mul(a, b);
    return spu_fm(a, b);
}
HAZE_AUDIO_INLINE u128 haze_audio_fma(u128 a, u128 b, u128 c) {
    if (spu_float_native_active()) return spu_float_native_madd(a, b, c);
    return spu_fma(a, b, c);
}
HAZE_AUDIO_INLINE u128 haze_audio_fms(u128 a, u128 b, u128 c) {
    if (spu_float_native_active()) {
        for (unsigned i = 0; i < 4; ++i) c._u32[i] ^= 0x80000000u;
        return spu_float_native_madd(a, b, c);
    }
    return spu_fms(a, b, c);
}
HAZE_AUDIO_INLINE u128 haze_audio_fnms(u128 a, u128 b, u128 c) {
    if (spu_float_native_active()) {
        for (unsigned i = 0; i < 4; ++i) a._u32[i] ^= 0x80000000u;
        return spu_float_native_madd(a, b, c);
    }
    return spu_fnms(a, b, c);
}
#undef HAZE_AUDIO_INLINE

#ifndef HAZE_AUDIO_FLOAT_NO_REMAP
#define spu_fa haze_audio_fa
#define spu_fs haze_audio_fs
#define spu_fm haze_audio_fm
#define spu_fma haze_audio_fma
#define spu_fms haze_audio_fms
#define spu_fnms haze_audio_fnms
#endif
