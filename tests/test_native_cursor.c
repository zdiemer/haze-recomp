/* Compare the native operation's audited caller-visible contract with the
 * actual locally extracted translation. Volatile r2..r37 are intentionally
 * discarded only on accepted native calls; fallback compares every byte. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <xmmintrin.h>
#include "haze_native_cursor_bridge.h"

void haze_cursor_reference_000047E0(spu_context*);
void haze_cursor_candidate_000047E0(spu_context*);
void haze_cursor_reference_00004900(spu_context*);
void haze_cursor_candidate_00004900(spu_context*);
int g_spu_ls_watch_n;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
static unsigned enabled=1,hits;
int haze_native_cursor_enabled(void) { return enabled; }
void haze_native_cursor_note(void) { ++hits; }
void spu_ls_watch_slow(uint32_t a,int b,const uint8_t* p,uint32_t pc,uint32_t lr)
{ (void)a;(void)b;(void)p;(void)pc;(void)lr; }
void spu_drain_call(spu_context* ctx,uint32_t pc)
{
    // The extracted leaf has no branches/channels; the real drain immediately
    // recognizes its matching PC, resets drain_ret_pc and returns.
    assert(ctx->pc==pc && !g_spu_trampoline_fn); ctx->drain_ret_pc=0;
}
void spu_img_restore(spu_context* ctx,int32_t image) { assert(ctx->image_id==image); }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }
static spu_context original,native;
static uint32_t seed=0x72b9a315;
static uint32_t random_word(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static void reset(unsigned site,uint32_t first,uint32_t second,unsigned iteration)
{
    memset(&original,0,sizeof(original)); original.ls=original.ls_store;
    for(unsigned i=0;i<sizeof(original.gpr)/4;++i) ((uint32_t*)original.gpr)[i]=random_word();
    for(unsigned i=0;i<SPU_LS_SIZE/4;++i) { uint32_t bits=random_word();memcpy(original.ls+4*i,&bits,4); }
    original.image_id=40;original.pc=site ? 0x4900 : 0x47E0;
    original.host_depth=iteration%4;original.drain_ret_pc=0x1234;
    original.gpr[site ? 89 : 88]._u32[0]=first;
    original.gpr[85]._u32[0]=second;
    if(iteration&1) original.gpr[site ? 83 : 82]._u32[0]=original.gpr[81]._u32[0];
    memcpy(&native,&original,sizeof(native)); native.ls=native.ls_store;
}
static void compare(unsigned site,int accepted)
{
    g_spu_trampoline_fn=NULL;
    if(site) haze_cursor_reference_00004900(&original); else haze_cursor_reference_000047E0(&original);
    void (*target)(spu_context*)=g_spu_trampoline_fn;
    g_spu_trampoline_fn=NULL; const unsigned before=hits;
    if(site) haze_cursor_candidate_00004900(&native); else haze_cursor_candidate_000047E0(&native);
    assert(target==g_spu_trampoline_fn && hits-before==(unsigned)accepted);
    if(accepted) {
        // These are the leaf's scratch outputs, dead at the audited boundary.
        // Memory, control transfer, all other registers and metadata must match.
        for(unsigned reg=2;reg<=37;++reg) {
            memset(&original.gpr[reg],0,16);memset(&native.gpr[reg],0,16);
        }
    }
    original.ls=native.ls;
    if(memcmp(&original,&native,sizeof(native))) {
        for(unsigned i=0;i<sizeof(native);++i) if(((uint8_t*)&original)[i]!=((uint8_t*)&native)[i]) {
            fprintf(stderr,"Cursor boundary mismatch site=%u accepted=%d byte=%u\n",site,accepted,i);break;
        }
        abort();
    }
}
static void guard_unchanged(unsigned site)
{
    memcpy(&original,&native,sizeof(native));
    assert(!haze_native_cursor_pair(&native,site ? 89 : 88,site ? 0x4910 : 0x47F0));
    assert(!memcmp(&original,&native,sizeof(native)));
}
static uint64_t cycles(void) { ULONG64 n; assert(QueryThreadCycleTime(GetCurrentThread(),&n));return n; }
int main(void)
{
    g_spu_ls_probe=0;g_spu_smc_watch=0;
    const uint32_t edges[]={0,4,8,12,SPU_LS_SIZE-24,SPU_LS_SIZE-16,0xFFFFFFF4};
    unsigned checks=0;
    for(unsigned site=0;site<2;++site) {
        for(unsigned a=0;a<sizeof(edges)/sizeof(*edges);++a)
            for(unsigned b=0;b<sizeof(edges)/sizeof(*edges);++b) {
                reset(site,edges[a],edges[b],a+b);
                compare(site,edges[a]<=SPU_LS_SIZE-24 && edges[b]<=SPU_LS_SIZE-24);++checks;
            }
        for(unsigned i=0;i<2048;++i) {
            uint32_t first=(random_word()%(SPU_LS_SIZE-64))&~3u;
            uint32_t second=i%8 ? first+(i%7)*4 : (random_word()%(SPU_LS_SIZE-64))&~3u;
            reset(site,first,second,i);compare(site,1);++checks;
        }
        reset(site,0x1000,0x1100,0);enabled=0;compare(site,0);enabled=1;++checks;
        reset(site,0x1001,0x1100,0);compare(site,0);++checks;
        reset(site,0x1000,0x1101,0);compare(site,0);++checks;
        reset(site,0x1000,0x1100,0);g_spu_trampoline_fn=NULL;
        native.image_id=41;guard_unchanged(site);native.image_id=40;
        native.policy_mode=1;guard_unchanged(site);native.policy_mode=0;
        native.pc++;guard_unchanged(site);native.pc--;
        native.ls=original.ls_store;guard_unchanged(site);native.ls=native.ls_store;
        g_spu_ls_probe=-1;guard_unchanged(site);g_spu_ls_probe=0;
        g_spu_ls_watch_n=1;guard_unchanged(site);g_spu_ls_watch_n=0;
        g_spu_smc_watch=1;guard_unchanged(site);g_spu_smc_watch=0;
    }
    // Preserve corrected scalar arithmetic across host rounding/denormal modes,
    // including extended finite values that host IEEE arithmetic calls NaNs.
    const uint32_t fp_pairs[][2]={
        {0x3F800000,0x3C23D70A},{0x3F800000,0x33000000},
        {0x3F800000,0x80000001},{0x3F800000,0xB3000000},
        {0x7F800000,0x3F800000},{0x7FFFFFFF,0x7FFFFFFF},
        {0x00800000,0x807FFFFF},{0x00000001,0x80000001},
        {0xBF800000,0x3F800000},{0xFF800000,0x7FFFFFFF}};
    const unsigned correctness_mxcsr=_mm_getcsr();
    for(unsigned mode=0;mode<8;++mode) {
        _mm_setcsr((correctness_mxcsr&~(0x6000u|0x8040u))|
            ((mode&3u)<<13)|(mode&4u?0x8040u:0));
        for(unsigned site=0;site<2;++site)
            for(unsigned pair=0;pair<sizeof(fp_pairs)/sizeof(*fp_pairs);++pair) {
                reset(site,0x1000,0x1100,pair);
                for(unsigned base=0x1000;base<=0x1100;base+=0x100)
                    for(unsigned field=0;field<6;++field) {
                        const uint32_t word=fp_pairs[pair][field&1];
                        haze_cursor_write_word(original.ls+base+4*field,word);
                        haze_cursor_write_word(native.ls+base+4*field,word);
                    }
                compare(site,1);++checks;
            }
    }
    _mm_setcsr(correctness_mxcsr);
    printf("Native cursor: %u boundary comparisons passed (aliasing, float bits, overflow, fallbacks); guard mutations unchanged\n",checks);
    reset(0,0x1000,0x1100,0);
    const uint32_t bits[]={0x3F800000,0x3C23D70A,0,100,0x40000000,0x3C23D70A};
    for(unsigned field=0;field<6;++field) {
        haze_cursor_write_word(original.ls+0x1000+4*field,bits[field]);
        haze_cursor_write_word(original.ls+0x1100+4*field,bits[field]);
        haze_cursor_write_word(native.ls+0x1000+4*field,bits[field]);
        haze_cursor_write_word(native.ls+0x1100+4*field,bits[field]);
    }
    const unsigned repeats=1000000;
    const unsigned saved_mxcsr=_mm_getcsr();
    for(unsigned mode=0;mode<2;++mode) {
    // Diagnostic only: normal cursor floats remain normal, while discarded
    // vector lanes reinterpret small integer counters as subnormal floats.
    // Never change the game's floating-point mode from this test result.
    _mm_setcsr(mode ? saved_mxcsr|0x8040u : saved_mxcsr&~0x8040u);
    for(unsigned run=0;run<4;++run) {
        uint64_t begin=cycles();
        for(unsigned i=0;i<repeats;++i) { g_spu_trampoline_fn=NULL; original.pc=0x47E0;haze_cursor_reference_000047E0(&original); }
        const uint64_t reference_cycles=cycles()-begin;
        begin=cycles();
        for(unsigned i=0;i<repeats;++i) { g_spu_trampoline_fn=NULL; native.pc=0x47E0;haze_cursor_candidate_000047E0(&native); }
        const uint64_t native_cycles=cycles()-begin;
        assert(!memcmp(original.ls,native.ls,SPU_LS_SIZE));
        printf("Caller-pair microbenchmark only: FTZ/DAZ=%u original %.3f cycles, native %.3f cycles, ratio %.3f\n",
               mode,(double)reference_cycles/repeats,(double)native_cycles/repeats,(double)reference_cycles/native_cycles);
    }
    }
    _mm_setcsr(saved_mxcsr);
}
