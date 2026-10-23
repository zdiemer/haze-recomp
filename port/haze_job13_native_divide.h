#ifndef HAZE_JOB13_NATIVE_DIVIDE_H
#define HAZE_JOB13_NATIVE_DIVIDE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "spu_context.h"

/* Compute the first 60 restoring-division steps on host integers. The final
 * original step still constructs the rounding state and all scratch registers.
 * Hook only after image32 EB10 initializes its quotient, counter and bit mask.
 * No guest memory, floating-point state, queues or publication are touched. */
static inline bool haze_job13_native_divide_prefix(spu_context* ctx) {
    static const uint8_t borrow_shuffle[16]={4,5,6,7,0xc0,0xc0,0xc0,0xc0,
                                           12,13,14,15,0xc0,0xc0,0xc0,0xc0};
    if (memcmp(ctx->ls+0xf2e0,borrow_shuffle,sizeof borrow_shuffle)) return false;
    for (unsigned i=0;i<4;++i)
        if (ctx->gpr[10]._u32[i] || ctx->gpr[12]._u32[i]) return false;
    if (ctx->gpr[11]._u32[0]!=0x10000000u || ctx->gpr[11]._u32[1] ||
        ctx->gpr[11]._u32[2]!=0x10000000u || ctx->gpr[11]._u32[3]) return false;
    const uint64_t divisor=((uint64_t)ctx->gpr[8]._u32[0]<<32)|ctx->gpr[8]._u32[1];
    uint64_t remainder=((uint64_t)ctx->gpr[9]._u32[0]<<32)|ctx->gpr[9]._u32[1];
    if ((ctx->gpr[5]._u32[0]==0)!=(remainder>=divisor)) return false;
    uint64_t quotient=0;
    for (unsigned bit=60;bit>0;--bit) {
        if (remainder>=divisor) {
            remainder-=divisor;
            quotient|=UINT64_C(1)<<bit;
        }
        remainder<<=1;
    }
    ctx->gpr[9]._u32[0]=(uint32_t)(remainder>>32);
    ctx->gpr[9]._u32[1]=(uint32_t)remainder;
    ctx->gpr[9]._u32[2]=ctx->gpr[9]._u32[3]=0;
    for (unsigned half=0;half<2;++half) {
        ctx->gpr[12]._u32[2*half]=(uint32_t)(quotient>>32);
        ctx->gpr[12]._u32[2*half+1]=(uint32_t)quotient;
        ctx->gpr[11]._u32[2*half]=0;
        ctx->gpr[11]._u32[2*half+1]=1;
    }
    for (unsigned i=0;i<4;++i) ctx->gpr[10]._u32[i]=60;
    return true;
}
#endif
