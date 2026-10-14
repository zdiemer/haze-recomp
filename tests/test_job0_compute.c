/* Private translated compute loops versus register-local code generation.
 * Checks the entire local store, all registers, and control-transfer state.
 * Synthetic inputs test equivalence; they are not representative FPS evidence. */
#include "spu_helpers.h"
#include "spu_float_native.h"
#include "haze_job0_kernel.h"
#include <windows.h>
#include <assert.h>

void spu_0029_at_00EC9A80_spu_func_00003408(spu_context*);
void spu_0029_at_00EC9A80_spu_func_000036F0(spu_context*);
void spu_0029_at_00EC9A80_spu_func_00003F48(spu_context*);
void job0_local_00003408(spu_context*);
void job0_local_000036F0(spu_context*);
void job0_local_00003F48(spu_context*);
void job0_inline_00003408(spu_context*);
void job0_inline_000036F0(spu_context*);
void job0_inline_00003F48(spu_context*);
void job0_integrated_00003408(spu_context*);
void job0_integrated_000036F0(spu_context*);
static int kernel_enabled=1;
static unsigned accepted;
int haze_job0_kernel_enabled(void) { return kernel_enabled; }
void haze_job0_kernel_note(unsigned pc) { assert(pc==0x3408 || pc==0x36f0);++accepted; }
int g_spu_ls_watch_n, g_wws_code_probe;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_ls_watch_slow(uint32_t a,int b,const uint8_t* p,uint32_t pc,uint32_t lr)
{ (void)a;(void)b;(void)p;(void)pc;(void)lr; abort(); }
static spu_context baseline, original, native;
static const unsigned pcs[] = {0x3408,0x36F0,0x3F48};
static const unsigned ends[] = {0x3500,0x3B38,0x41D0};
typedef void (*kernel)(spu_context*);
static kernel references[] = {spu_0029_at_00EC9A80_spu_func_00003408,
    spu_0029_at_00EC9A80_spu_func_000036F0,spu_0029_at_00EC9A80_spu_func_00003F48};
static kernel candidates[] = {job0_local_00003408,job0_local_000036F0,job0_local_00003F48};
static kernel inline_candidates[] = {job0_inline_00003408,job0_inline_000036F0,job0_inline_00003F48};
static kernel integrated[] = {job0_integrated_00003408,job0_integrated_000036F0,spu_0029_at_00EC9A80_spu_func_00003F48};
static uint32_t seed = 0x963041cd;
static uint32_t random_word(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static void synthetic(unsigned site,unsigned count)
{
    memset(&baseline,0,sizeof(baseline)); baseline.ls=baseline.ls_store;
    for(unsigned i=0;i<sizeof(baseline.gpr)/4;++i) ((uint32_t*)baseline.gpr)[i]=random_word();
    for(unsigned i=0;i<SPU_LS_SIZE/4;++i) { uint32_t n=random_word();memcpy(baseline.ls+4*i,&n,4); }
    baseline.image_id=30; baseline.pc=pcs[site];
    baseline.gpr[83]=spu_il(0x8000); baseline.gpr[84]=spu_il(0x4000);
    baseline.gpr[80]=spu_il(count); baseline.gpr[81]=spu_il(0);
    if(site==0) {
        baseline.gpr[86]=spu_il(count);baseline.gpr[16]=spu_il(0);
        baseline.gpr[85]=spu_il(0x10000);baseline.gpr[14]=spu_il(0x20000);
    } else if(site==1) {
        baseline.gpr[68]=spu_il(0x10000);baseline.gpr[72]=spu_il(0x20000);
    } else {
        baseline.gpr[45]=spu_il(0);baseline.gpr[40]=spu_il(0x10000);
        baseline.gpr[39]=spu_il(0x20000);
    }
}
static void reset(spu_context* ctx)
{ memcpy(ctx,&baseline,sizeof(*ctx));ctx->ls=ctx->ls_store; }
static void run(spu_context* ctx,unsigned site,int candidate)
{
    g_spu_trampoline_fn=NULL;
    (candidate==3?integrated[site]:candidate==2?inline_candidates[site]:candidate?candidates[site]:references[site])(ctx);
    unsigned limit=65536;
    while(ctx->pc!=ends[site]) {
        assert(limit-- && g_spu_trampoline_fn);
        kernel next=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;next(ctx);
    }
}
static void verify(unsigned site,int candidate)
{
    reset(&original);reset(&native);
    run(&original,site,0);kernel target=g_spu_trampoline_fn;
    run(&native,site,candidate);assert(g_spu_trampoline_fn==target);
    original.ls=native.ls;
    if(memcmp(&original,&native,sizeof(native))) {
        for(unsigned i=0;i<sizeof(native);++i) if(((uint8_t*)&original)[i]!=((uint8_t*)&native)[i]) {
            fprintf(stderr,"Mismatch pc=%x byte=%u original=%02x native=%02x\n",pcs[site],i,
                ((uint8_t*)&original)[i],((uint8_t*)&native)[i]);break;
        }
        abort();
    }
}
static uint64_t cycles(void) { ULONG64 n;assert(QueryThreadCycleTime(GetCurrentThread(),&n));return n; }
static void benchmark(unsigned site,const char* label,int variant)
{
    uint64_t costs[2]={0,0};
    // Copies are outside each measured batch except the 2-KiB register reset.
    // Alternate ordering to reduce sustained-clock/order bias.
    for(unsigned round=0;round<8;++round) for(unsigned pass=0;pass<2;++pass) {
        unsigned candidate=(round+pass)&1;reset(&native);
        uint64_t start=cycles();
        for(unsigned i=0;i<64;++i) {
            memcpy(native.gpr,baseline.gpr,sizeof(native.gpr));native.pc=pcs[site];
            run(&native,site,candidate?variant:0);
        }
        costs[candidate]+=cycles()-start;
    }
    printf("%s pc=%x variant=%d original_cycles/call=%.1f candidate_cycles/call=%.1f ratio=%.3f\n",
        label,pcs[site],variant,costs[0]/512.0,costs[1]/512.0,(double)costs[0]/costs[1]);
}
static uint32_t le32(const uint8_t* p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t le64(const uint8_t* p)
{ return (uint64_t)le32(p)|((uint64_t)le32(p+4)<<32); }
static unsigned load_fixture(const char* path)
{
    FILE* f=fopen(path,"rb");assert(f);uint8_t header[56];
    assert(fread(header,1,sizeof(header),f)==sizeof(header));
    assert(!memcmp(header,"HZJ0CAP\0",8) && le32(header+8)==1 && le32(header+12)==56);
    assert(le32(header+20)==sizeof(baseline.gpr) && le32(header+24)==SPU_LS_SIZE && !le32(header+52));
    unsigned site=0;while(site<3 && pcs[site]!=le32(header+16)) ++site;assert(site<3);
    memset(&baseline,0,sizeof(baseline));baseline.ls=baseline.ls_store;
    baseline.image_id=30;baseline.pc=pcs[site];
    assert(fread(baseline.gpr,1,sizeof(baseline.gpr),f)==sizeof(baseline.gpr));
    assert(fread(baseline.ls,1,SPU_LS_SIZE,f)==SPU_LS_SIZE);
    assert(fgetc(f)==EOF);fclose(f);
    uint64_t hash=14695981039346656037ull;
    for(unsigned part=0;part<2;++part) {
        const uint8_t* p=part?baseline.ls:(const uint8_t*)baseline.gpr;
        unsigned n=part?SPU_LS_SIZE:sizeof(baseline.gpr);
        for(unsigned i=0;i<n;++i) { hash^=p[i];hash*=1099511628211ull; }
    }
    assert(hash==le64(header+44));
    unsigned limit=baseline.gpr[site?80:86]._u32[0];
    unsigned counter=baseline.gpr[site==0?16:site==1?81:45]._u32[0];
    assert(limit>counter && limit-counter<=65536);
    printf("fixture=%s pc=%x remaining_items=%u\n",path,pcs[site],limit-counter);
    return site;
}
int main(int argc,char** argv)
{
    g_spu_ls_probe=g_spu_smc_watch=0;
    const unsigned counts[]={1,2,3,8,31,64};unsigned checks=0;
    for(unsigned mode=0;mode<2;++mode) {
        if(mode && !spu_float_native_available()) continue;
        spu_float_native_set_mode(mode);
        for(unsigned site=0;site<3;++site) {
            for(unsigned i=0;i<96;++i) {
                synthetic(site,counts[i%6]);verify(site,1);++checks;
                if(mode) { verify(site,2);++checks; }
                if(site<2) {
                    unsigned before=accepted;verify(site,3);++checks;
                    assert(accepted-before==mode);
                }
            }
            synthetic(site,64);benchmark(site,mode?"synthetic hardware arithmetic":"synthetic scalar arithmetic",1);
            if(mode) benchmark(site,"synthetic inline arithmetic",2);
        }
    }
    for(int file=1;file<argc;++file) {
        unsigned site=load_fixture(argv[file]);
        for(unsigned mode=0;mode<2;++mode) {
            if(mode && !spu_float_native_available()) continue;
            spu_float_native_set_mode(mode);verify(site,1);++checks;
            if(mode) { verify(site,2);++checks; }
            if(site<2) { unsigned before=accepted;verify(site,3);++checks;assert(accepted-before==mode); }
        }
        benchmark(site,spu_float_native_active()?"captured hardware arithmetic":"captured scalar arithmetic",1);
        if(spu_float_native_active()) benchmark(site,"captured inline arithmetic",2);
    }
    if(spu_float_native_active()) {
        synthetic(0,4);reset(&native);g_spu_trampoline_fn=NULL;
        assert(haze_job0_kernel_accept(&native,0x3408));
        kernel_enabled=0;assert(!haze_job0_kernel_accept(&native,0x3408));kernel_enabled=1;
        native.image_id=31;assert(!haze_job0_kernel_accept(&native,0x3408));native.image_id=30;
        native.policy_mode=1;assert(!haze_job0_kernel_accept(&native,0x3408));native.policy_mode=0;
        native.pc++;assert(!haze_job0_kernel_accept(&native,0x3408));native.pc--;
        native.ls=baseline.ls;assert(!haze_job0_kernel_accept(&native,0x3408));native.ls=native.ls_store;
        g_spu_trampoline_fn=references[0];assert(!haze_job0_kernel_accept(&native,0x3408));g_spu_trampoline_fn=NULL;
        g_spu_ls_probe=1;assert(!haze_job0_kernel_accept(&native,0x3408));g_spu_ls_probe=0;
        g_spu_smc_watch=1;assert(!haze_job0_kernel_accept(&native,0x3408));g_spu_smc_watch=0;
        g_spu_ls_watch_n=1;assert(!haze_job0_kernel_accept(&native,0x3408));g_spu_ls_watch_n=0;
    }
    printf("%u full-state compute comparisons passed\n",checks);return 0;
}
