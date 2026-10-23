/* Private whole-batch replay: explicitly labeled compiled computation, recorded channel
 * inputs, and exact comparisons of all channel writes, DMA PUTs and final state.
 * This deliberately has no real guest memory or SDK scheduling side effects.
 * It validates the compute boundary, not ownership/completion integration. */
#include "haze_job13_capture.h"
#include "spu_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef HAZE_JOB13_REPLAY_VARIANT
#define HAZE_JOB13_REPLAY_VARIANT "unspecified"
#endif

static haze_job13_capture_header header;
static haze_job13_capture_event *events;
static uint8_t *payload;
static unsigned cursor;
static uint64_t steps;
static int atomic_seen;
static void (*functions[SPU_LS_SIZE/4])(spu_context*);
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;
int g_spu_ls_watch_n = 0;

static void fail(const char *message) {
    fprintf(stderr, "REJECT event=%u: %s\n", cursor, message);
    exit(2);
}
static void require(int condition, const char *message) {
    if (!condition) fail(message);
}
static void equal_bytes(const void *a, const void *b, size_t size, const char *name) {
    const uint8_t *x=(const uint8_t*)a, *y=(const uint8_t*)b;
    for(size_t i=0;i<size;++i) if(x[i]!=y[i]) {
        fprintf(stderr,"Mismatch %s byte=%zu actual=%02x expected=%02x\n",name,i,x[i],y[i]);
        fail(name);
    }
}
static const haze_job13_capture_event *next_event(unsigned kind, unsigned channel) {
    require(cursor<header.event_count,"unrecorded channel operation");
    const haze_job13_capture_event *e=&events[cursor];
    require(e->sequence==cursor && e->kind==kind && e->channel==channel,"channel sequence changed");
    ++cursor;
    return e;
}
void spu_wrch(spu_context *ctx,uint32_t channel,u128 value) {
    const haze_job13_capture_event *e=next_event(HAZE_JOB13_EVENT_WRCH,channel);
    equal_bytes(&value,e->value,16,"channel write");
    switch(channel) {
    case MFC_LSA: ctx->mfc_lsa=value._u32[0]; break;
    case MFC_EAH: ctx->mfc_eah=value._u32[0]; break;
    case MFC_EAL: ctx->mfc_eal=value._u32[0]; break;
    case MFC_Size: ctx->mfc_size=value._u32[0]; break;
    case MFC_TagID: ctx->mfc_tag=value._u32[0]&31; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask=value._u32[0]; break;
    case MFC_WrTagUpdate: break; /* Returned status comes from the exact read. */
    case MFC_Cmd:
        require(e->command==value._u32[0] && e->lsa==ctx->mfc_lsa &&
            e->eah==ctx->mfc_eah && e->eal==ctx->mfc_eal &&
            e->size==ctx->mfc_size && e->tag==ctx->mfc_tag,"DMA operands changed");
        if(e->command==MFC_GETLLAR_CMD || e->command==MFC_PUTLLC_CMD) {
            /* Match this SDK's actual line operation, preserving raw staged
             * operands above: EA is low32 aligned down, LSA is masked only,
             * and the transfer is always 128 bytes regardless of MFC_Size.
             * Success/failure is an observed input, not a simulated proof of
             * concurrent queue ownership. Compare every attempted PUT too. */
            uint32_t lsa=e->lsa & SPU_LS_MASK;
            uint32_t ea=e->eal & ~(MFC_ATOMIC_LINE-1u);
            require(lsa<=SPU_LS_SIZE-MFC_ATOMIC_LINE &&
                e->payload_length==MFC_ATOMIC_LINE,"invalid atomic span");
            if(e->command==MFC_GETLLAR_CMD) {
                require(e->atomic_status==4,"invalid GETLLAR status");
                memcpy(ctx->ls+lsa,payload+e->payload_offset,MFC_ATOMIC_LINE);
                memcpy(ctx->resv_line,ctx->ls+lsa,MFC_ATOMIC_LINE);
                ctx->resv_ea=ea;ctx->resv_valid=1;atomic_seen=1;
            } else {
                require(atomic_seen && e->atomic_status<=1,"invalid PUTLLC status/order");
                if(!e->atomic_status) require(ctx->resv_valid && ctx->resv_ea==ea,
                    "successful PUTLLC without matching reservation");
                equal_bytes(ctx->ls+lsa,payload+e->payload_offset,MFC_ATOMIC_LINE,"atomic PUT attempt");
                ctx->resv_valid=0;
            }
            ctx->atomic_stat=e->atomic_status;
        } else if(e->command>=0x40 && e->command<=0x42) {
            require(e->lsa<=SPU_LS_SIZE && e->size<=SPU_LS_SIZE-e->lsa &&
                e->payload_length==e->size,"invalid DMA local-store span");
            if(e->size) memcpy(ctx->ls+e->lsa,payload+e->payload_offset,e->size);
        } else if(e->command>=0x20 && e->command<=0x22) {
            require(e->lsa<=SPU_LS_SIZE && e->size<=SPU_LS_SIZE-e->lsa &&
                e->payload_length==e->size,"invalid DMA local-store span");
            if(e->size) equal_bytes(ctx->ls+e->lsa,payload+e->payload_offset,e->size,"DMA PUT");
        }
        else fail("unsupported DMA command");
        break;
    default: fail("unsupported channel write");
    }
}
u128 spu_rdch(spu_context *ctx,uint32_t channel) {
    const haze_job13_capture_event *e=next_event(HAZE_JOB13_EVENT_RDCH,channel);
    u128 value; memcpy(&value,e->value,16);
    require(channel==MFC_RdTagStat || channel==MFC_RdTagMask || channel==MFC_RdAtomicStat,
        "unsupported channel read");
    if(channel==MFC_RdAtomicStat) require(atomic_seen && value._u32[0]==ctx->atomic_stat,
        "atomic result differs from recorded operation");
    if(channel==MFC_RdTagStat) ctx->mfc_tag_status=value._u32[0];
    return value;
}
uint32_t spu_rchcnt(spu_context *ctx,uint32_t channel) {
    (void)ctx;
    require(channel==MFC_RdTagStat || channel==MFC_RdTagMask || channel==MFC_RdAtomicStat,
        "unsupported channel count");
    const haze_job13_capture_event *e=next_event(HAZE_JOB13_EVENT_RCHCNT,channel);
    uint32_t result; memcpy(&result,e->value,4); return result;
}
void spu_register_function(uint32_t address,void (*fn)(spu_context*)) {
    require(address<SPU_LS_SIZE && !(address&3),"invalid registered function");
    require(!functions[address/4],"duplicate registered function"); functions[address/4]=fn;
}
void spu_indirect_branch(spu_context *ctx) {
    require(ctx->image_id==32 && ctx->pc<SPU_LS_SIZE && !(ctx->pc&3),"foreign image or invalid branch");
    require(functions[ctx->pc/4]!=NULL,"unrecorded external branch");
    if(++steps>10000000) fail("replay transfer bound exceeded");
    functions[ctx->pc/4](ctx);
}
void spu_drain_call(spu_context *ctx,uint32_t return_pc) {
    require(ctx->host_depth<256,"replay call depth exceeded");
    for(;;) {
        if(ctx->pc==return_pc && (!g_spu_trampoline_fn || g_spu_trampoline_fn==spu_indirect_branch)) {
            g_spu_trampoline_fn=NULL; return;
        }
        void (*fn)(spu_context*)=g_spu_trampoline_fn;
        g_spu_trampoline_fn=NULL;
        if(++steps>10000000) fail("replay transfer bound exceeded");
        if(fn) fn(ctx); else spu_indirect_branch(ctx);
    }
}
void spu_img_restore(spu_context *ctx,int32_t image) {
    require(image==32 && ctx->image_id==32,"image switch inside compute batch");
}
void spu_ls_watch_slow(uint32_t a,int w,const uint8_t *p,uint32_t pc,uint32_t lr) {
    (void)a;(void)w;(void)p;(void)pc;(void)lr;fail("unexpected LS watch callback");
}
void spu_halt(spu_context *ctx) { (void)ctx;fail("guest halt"); }
void spu_stop(spu_context *ctx) { (void)ctx;fail("guest syscall outside compute boundary"); }

static void read_exact(FILE *f,void *p,size_t bytes) {
    require(fread(p,1,bytes,f)==bytes,"truncated fixture");
}
int main(int argc,char **argv) {
    if(argc!=2 && argc!=3) {fprintf(stderr,"Usage: replay_job13_capture fixture.bin [repetitions]\n");return 2;}
    unsigned repetitions=1;
    if(argc==3) {
        char *end=NULL;unsigned long n=strtoul(argv[2],&end,10);
        require(end && !*end && n>=2 && n<=1000,"repetitions must be 2..1000");
        repetitions=(unsigned)n;
    }
    FILE *f=fopen(argv[1],"rb"); require(f!=NULL,"cannot open fixture");
    read_exact(f,&header,sizeof header);
    require(!memcmp(header.magic,"HZJOB13\0",8) && header.version==HAZE_JOB13_CAPTURE_VERSION &&
        header.header_size==sizeof header,"unsupported fixture format");
    require(header.valid==1 && header.invalid_reason==0 && header.image_id==32 &&
        header.final_pc==0xCE04 && header.initial_status==header.final_status,"incomplete/invalid batch");
    require(!(header.initial_mxcsr&~0xffffu) && !(header.final_mxcsr&~0xffffu),"invalid MXCSR");
    require(header.event_count>0 && header.event_count<=HAZE_JOB13_CAPTURE_MAX_EVENTS &&
        header.event_calls==header.event_count && header.payload_size<=HAZE_JOB13_CAPTURE_MAX_PAYLOAD,
        "invalid fixture bounds");
    spu_context *ctx=(spu_context*)_aligned_malloc(sizeof *ctx,16);
    uint8_t *expected_gpr=(uint8_t*)malloc(HAZE_JOB13_CAPTURE_GPR_BYTES);
    uint8_t *expected_ls=(uint8_t*)malloc(SPU_LS_SIZE);
    events=(haze_job13_capture_event*)calloc(header.event_count,sizeof *events);
    payload=(uint8_t*)malloc(header.payload_size ? header.payload_size : 1);
    require(ctx && expected_gpr && expected_ls && events && payload,"allocation failure");
    spu_context_init(ctx,0);
    read_exact(f,ctx->gpr,sizeof ctx->gpr); read_exact(f,expected_gpr,sizeof ctx->gpr);
    read_exact(f,ctx->ls,SPU_LS_SIZE); read_exact(f,expected_ls,SPU_LS_SIZE);
    read_exact(f,events,header.event_count*sizeof *events);read_exact(f,payload,header.payload_size);
    require(fgetc(f)==EOF,"trailing fixture data"); fclose(f);
    uint32_t consumed=0;
    for(unsigned i=0;i<header.event_count;++i) {
        const haze_job13_capture_event *e=&events[i];
        require(e->sequence==i && e->kind>=1 && e->kind<=3,"invalid event");
        int atomic=e->kind==HAZE_JOB13_EVENT_WRCH && e->channel==MFC_Cmd &&
            (e->command==MFC_GETLLAR_CMD || e->command==MFC_PUTLLC_CMD);
        require(atomic || !e->atomic_status,"atomic status on unrelated event");
        if(e->payload_length) {
            require(e->payload_offset<=header.payload_size && e->payload_length<=header.payload_size-e->payload_offset,
                "invalid payload span");
            require(e->kind==HAZE_JOB13_EVENT_WRCH && e->channel==MFC_Cmd &&
                e->payload_offset==consumed,"noncanonical DMA payload");
            consumed+=e->payload_length;
        } else require(e->payload_offset==UINT32_MAX,"noncanonical empty payload");
    }
    require(consumed==header.payload_size,"unused payload");
    ctx->image_id=32;ctx->pc=header.initial_pc;ctx->status=header.initial_status;ctx->host_depth=1;
    ctx->mfc_lsa=header.initial_lsa;ctx->mfc_eah=header.initial_eah;ctx->mfc_eal=header.initial_eal;
    ctx->mfc_size=header.initial_size;ctx->mfc_tag=header.initial_tag;
    ctx->mfc_tag_mask=header.initial_tag_mask;ctx->mfc_tag_status=header.initial_tag_status;
    ctx->atomic_stat=header.initial_atomic_stat;atomic_seen=1;
    require(ctx->gpr[0]._u32[0]==0xCE04,"unexpected batch continuation");
    spu_0031_at_00ECEC00_spu_recomp_register();
    spu_context *initial=(spu_context*)_aligned_malloc(sizeof *initial,16);
    require(initial!=NULL,"allocate initial state");memcpy(initial,ctx,sizeof *initial);
    int64_t durations[1000];LARGE_INTEGER frequency;
    require(QueryPerformanceFrequency(&frequency)!=0,"performance frequency");
    for(unsigned repeat=0;repeat<repetitions;++repeat) {
    memcpy(ctx,initial,sizeof *ctx);
    cursor=0;steps=0;atomic_seen=1;g_spu_trampoline_fn=NULL;
    LARGE_INTEGER begin,end;
    _mm_setcsr(header.initial_mxcsr);
    require(QueryPerformanceCounter(&begin)!=0,"begin timer");
    spu_0031_at_00ECEC00_spu_func_00008AD8(ctx);
    spu_drain_call(ctx,0xCE04);
    unsigned final_mxcsr=_mm_getcsr();
    require(QueryPerformanceCounter(&end)!=0,"end timer");
    durations[repeat]=end.QuadPart-begin.QuadPart;
    require(cursor==header.event_count,"unconsumed events");
    require(ctx->pc==header.final_pc && ctx->status==header.final_status && ctx->image_id==32,"final continuation mismatch");
#ifdef HAZE_JOB13_COMPARE_FINAL
    HAZE_JOB13_COMPARE_FINAL(ctx,initial,expected_gpr,expected_ls);
#else
    equal_bytes(ctx->gpr,expected_gpr,sizeof ctx->gpr,"final registers");
    equal_bytes(ctx->ls,expected_ls,SPU_LS_SIZE,"final local store");
#endif
    require(final_mxcsr==header.final_mxcsr,"floating-point status mismatch");
    }
    printf("PASS whole-batch replay variant=%s events=%u payload=%u steps=%llu\n",
        HAZE_JOB13_REPLAY_VARIANT,cursor,header.payload_size,(unsigned long long)steps);
    if(repetitions>1) {
        /* First iteration warms code/data. Input reset and final-state checks
         * are outside timed compute; ordered channel/PUT comparisons remain
         * inside and identical for both builds. Never report this as game FPS. */
        unsigned n=repetitions-1;int64_t total=0;
        for(unsigned i=1;i<repetitions;++i) total+=durations[i];
        for(unsigned i=2;i<repetitions;++i) {
            int64_t value=durations[i];unsigned j=i;
            while(j>1 && durations[j-1]>value) {durations[j]=durations[j-1];--j;}
            durations[j]=value;
        }
        double median=(n&1)?(double)durations[1+n/2]:
            ((double)durations[n/2]+(double)durations[1+n/2])*.5;
        printf("BENCH samples=%u mean_ms=%.6f median_ms=%.6f min_ms=%.6f max_ms=%.6f variant=%s\n",
            n,1000.0*total/n/frequency.QuadPart,1000.0*median/frequency.QuadPart,
            1000.0*durations[1]/frequency.QuadPart,1000.0*durations[n]/frequency.QuadPart,
            HAZE_JOB13_REPLAY_VARIANT);
    }
    _aligned_free(initial);_aligned_free(ctx);free(expected_gpr);free(expected_ls);free(events);free(payload);
    return 0;
}
