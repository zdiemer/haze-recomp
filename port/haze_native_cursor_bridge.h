#pragma once
#include "spu_helpers.h"
#include "haze_native_cursor.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
int haze_native_cursor_enabled(void);
void haze_native_cursor_note(void);
#ifdef __cplusplus
}
#endif

static inline uint32_t haze_cursor_read_word(const uint8_t* p)
{
    uint32_t bits; memcpy(&bits,p,4); return SPU_BSWAP32(bits);
}
static inline void haze_cursor_write_word(uint8_t* p,uint32_t bits)
{
    bits=SPU_BSWAP32(bits); memcpy(p,&bits,4);
}
static inline void haze_cursor_advance_buffer(uint8_t* data)
{
    // Compatibility conversion lives at the boundary, outside the PC kernel.
    haze_native_cursor cursor;
    uint32_t words[6];
    for(unsigned i=0;i<6;++i) words[i]=haze_cursor_read_word(data+4*i);
    memcpy(&cursor,words,sizeof(cursor));
    haze_native_cursor_advance(&cursor,spu_float_add);
    memcpy(words,&cursor,sizeof(cursor));
    const unsigned changed[]={0,8,12,16};
    for(unsigned i=0;i<4;++i) haze_cursor_write_word(data+changed[i],words[changed[i]/4]);
}

// Only the two audited caller blocks use this replacement. Their continuations
// do not consume the leaf's volatile r2..r37 results. The leaf remains present
// for indirect calls, diagnostics, unsupported pointers and original mode.
static inline int haze_native_cursor_pair(spu_context* ctx,unsigned first_reg,uint32_t return_pc)
{
    if(ctx->image_id!=40 || ctx->policy_mode || ctx->ls!=ctx->ls_store ||
       g_spu_ls_watch_n!=0 || g_spu_ls_probe!=0 || g_spu_smc_watch!=0 ||
       g_spu_trampoline_fn ||
       !((ctx->pc==0x47E0 && first_reg==88 && return_pc==0x47F0) ||
         (ctx->pc==0x4900 && first_reg==89 && return_pc==0x4910))) return 0;
    const uint32_t first=ctx->gpr[first_reg]._u32[0],second=ctx->gpr[85]._u32[0];
    if(((first|second)&3) || first>SPU_LS_SIZE-24 || second>SPU_LS_SIZE-24)
        return 0;
    // Both pointers are validated before either write. Preserve pair ordering
    // even for identical/overlapping cursors.
    haze_cursor_advance_buffer(ctx->ls+first);
    haze_cursor_advance_buffer(ctx->ls+second);
    memset(&ctx->gpr[0],0,sizeof(ctx->gpr[0])); ctx->gpr[0]._u32[0]=return_pc;
    ctx->pc=return_pc;
    ctx->drain_ret_pc=0;
    return 1;
}
