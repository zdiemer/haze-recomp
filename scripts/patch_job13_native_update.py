"""Install an original-retaining shadow/execute wrapper for the 09A00 loop."""
import argparse
import hashlib
from pathlib import Path

from patch_job13_capture import PREFIX, REGISTRATION
from patch_job13_record_loop import patch as fuse_loop

ENTRY = '00009A00'
EXIT = '00009BC4'
EXPECTED_ENTRY_BODY = '9f88830479ba5d357eb8a64b997410da95a231cd8a6c91ba1d4d913a9b9cca08'

WRAPPER = r'''
/* BEGIN JOB13 NATIVE UPDATE WRAPPER */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "haze_job13_native_update.h"

#ifndef HAZE_JOB13_NATIVE_UPDATE_MODE
#define HAZE_JOB13_NATIVE_UPDATE_MODE 0 /* 0=reference, 1=shadow, 2=execute */
#endif
#ifndef HAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS
#define HAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS 1
#endif
#if HAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS
#define HAZE_JOB13_UPDATE_COUNT(...) do { __VA_ARGS__; } while (0)
#else
#define HAZE_JOB13_UPDATE_COUNT(...) ((void)0)
#endif
static _Atomic uint64_t haze_job13_update_attempts;
static _Atomic uint64_t haze_job13_update_mismatches;
static _Atomic uint64_t haze_job13_update_calls;
static _Atomic uint64_t haze_job13_update_particles;
static _Atomic uint64_t haze_job13_update_rejected_mutations;
static _Atomic uint64_t haze_job13_update_fallbacks[32];
static _Atomic int haze_job13_update_mismatch_reported;
static _Atomic uint32_t haze_job13_update_entry_sp;
extern uint32_t haze_job13_native_update_last_reject(void);

uint64_t haze_job13_update_call_count(void) { return atomic_load_explicit(&haze_job13_update_calls,memory_order_relaxed); }
uint64_t haze_job13_update_accept_count(void) { return haze_job13_native_update_shadow_attempts(); }
uint64_t haze_job13_update_particle_count(void) { return atomic_load_explicit(&haze_job13_update_particles,memory_order_relaxed); }
uint64_t haze_job13_update_fallback_count(unsigned reason) {
    return reason<32 ? atomic_load_explicit(&haze_job13_update_fallbacks[reason],memory_order_relaxed) : 0;
}
uint64_t haze_job13_update_rejected_mutation_count(void) {
    return atomic_load_explicit(&haze_job13_update_rejected_mutations,memory_order_relaxed);
}
uint32_t haze_job13_update_entry_stack_pointer(void) {
    return atomic_load_explicit(&haze_job13_update_entry_sp,memory_order_relaxed);
}

static int haze_job13_update_equal(const spu_context* a, const spu_context* b) {
    const uint32_t object=0x108c0u;
    const uint32_t capacity_ptr=a->gpr[107]._u32[0];
    if (capacity_ptr<0x414u || capacity_ptr>SPU_LS_SIZE-4u) return 0;
    const uint8_t* cp=a->ls+capacity_ptr;
    const uint32_t capacity=((uint32_t)cp[0]<<24)|((uint32_t)cp[1]<<16)|((uint32_t)cp[2]<<8)|cp[3];
    if (capacity>4096u || (uint64_t)capacity*40u>SPU_LS_SIZE-0x10d00u) return 0;
    if (a->pc!=0x9bc4u || b->pc!=0x9bc4u || a->status!=b->status) return 0;
    if (memcmp(a->ls+object,b->ls+object,0x440u)) return 0;
    if (memcmp(a->ls+0x10d00u,b->ls+0x10d00u,(size_t)capacity*40u)) return 0;
    static const unsigned liveins[]={1,94,107,115,119,120,121,122};
    for (unsigned i=0;i<sizeof liveins/sizeof liveins[0];++i)
        if (memcmp(&a->gpr[liveins[i]],&b->gpr[liveins[i]],sizeof a->gpr[0])) return 0;
    return 1;
}

uint64_t haze_job13_native_update_shadow_attempts(void) {
    return atomic_load_explicit(&haze_job13_update_attempts,memory_order_relaxed);
}
uint64_t haze_job13_native_update_shadow_mismatches(void) {
    return atomic_load_explicit(&haze_job13_update_mismatches,memory_order_relaxed);
}

void spu_0031_at_00ECEC00_spu_func_00009A00(spu_context* ctx) {
#if HAZE_JOB13_NATIVE_UPDATE_MODE == 2
    HAZE_JOB13_UPDATE_COUNT(atomic_fetch_add_explicit(&haze_job13_update_calls,1,memory_order_relaxed));
    HAZE_JOB13_UPDATE_COUNT(atomic_store_explicit(&haze_job13_update_entry_sp,ctx->gpr[1]._u32[0],memory_order_relaxed));
    if (haze_job13_native_update_try(ctx)) {
        HAZE_JOB13_UPDATE_COUNT(atomic_fetch_add_explicit(&haze_job13_update_attempts,1,memory_order_relaxed));
        const uint32_t sp=ctx->gpr[1]._u32[0];
        if (HAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS && sp<=SPU_LS_SIZE-0x15c4u) {
            const uint8_t* cp=ctx->ls+sp+0x15c0u;
            HAZE_JOB13_UPDATE_COUNT(atomic_fetch_add_explicit(&haze_job13_update_particles,
                ((uint64_t)cp[0]<<24)|((uint64_t)cp[1]<<16)|((uint64_t)cp[2]<<8)|cp[3],memory_order_relaxed));
        }
        ctx->pc=0x9bc4u;
        g_spu_trampoline_fn=spu_0031_at_00ECEC00_spu_func_00009BC4;
        return;
    }
    const uint32_t reason=haze_job13_native_update_last_reject();
    if (HAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS && reason<32u) HAZE_JOB13_UPDATE_COUNT(atomic_fetch_add_explicit(&haze_job13_update_fallbacks[reason],1,memory_order_relaxed));
    spu_0031_at_00ECEC00_spu_func_00009A00_reference(ctx);
#elif HAZE_JOB13_NATIVE_UPDATE_MODE == 0
    HAZE_JOB13_UPDATE_COUNT(atomic_store_explicit(&haze_job13_update_entry_sp,ctx->gpr[1]._u32[0],memory_order_relaxed));
    spu_0031_at_00ECEC00_spu_func_00009A00_reference(ctx);
#elif HAZE_JOB13_NATIVE_UPDATE_MODE == 1
    atomic_fetch_add_explicit(&haze_job13_update_calls,1,memory_order_relaxed);
    const unsigned original_mxcsr=_mm_getcsr();
    spu_context* candidate=(spu_context*)malloc(sizeof *candidate);
    spu_context* reference=(spu_context*)malloc(sizeof *reference);
    if (candidate && reference) {
        memcpy(candidate,ctx,sizeof *candidate);
        memcpy(candidate->ls_store,ctx->ls,SPU_LS_SIZE);
        candidate->ls=candidate->ls_store;
        _mm_setcsr(original_mxcsr);
        if (haze_job13_native_update_try(candidate)) {
            atomic_fetch_add_explicit(&haze_job13_update_attempts,1,memory_order_relaxed);
            const uint32_t sp=ctx->gpr[1]._u32[0];
            if (sp<=SPU_LS_SIZE-0x15c4u) {
                const uint8_t* cp=ctx->ls+sp+0x15c0u;
                atomic_fetch_add_explicit(&haze_job13_update_particles,
                    ((uint64_t)cp[0]<<24)|((uint64_t)cp[1]<<16)|((uint64_t)cp[2]<<8)|cp[3],memory_order_relaxed);
            }
            const unsigned candidate_mxcsr=_mm_getcsr();
            candidate->pc=0x9bc4u;
            memcpy(reference,ctx,sizeof *reference);
            memcpy(reference->ls_store,ctx->ls,SPU_LS_SIZE);
            reference->ls=reference->ls_store;
            _mm_setcsr(original_mxcsr);
            haze_job13_fused_records(reference);
            const unsigned reference_mxcsr=_mm_getcsr();
            /* Compare outputs independently of dead loop scratch/volatile GPRs. */
            if (!haze_job13_update_equal(candidate,reference) || candidate_mxcsr!=reference_mxcsr) {
                atomic_fetch_add_explicit(&haze_job13_update_mismatches,1,memory_order_relaxed);
                if (!atomic_exchange_explicit(&haze_job13_update_mismatch_reported,1,memory_order_relaxed)) {
                    const uint8_t* cp=candidate->ls+candidate->gpr[107]._u32[0];
                    const unsigned capacity=((unsigned)cp[0]<<24)|((unsigned)cp[1]<<16)|((unsigned)cp[2]<<8)|cp[3];
                    unsigned at=0;
                    while (at<0x440u && candidate->ls[0x108c0u+at]==reference->ls[0x108c0u+at]) ++at;
                    if (at<0x440u) fprintf(stderr,"native_update_diff=object+%x %02x/%02x ",at,candidate->ls[0x108c0u+at],reference->ls[0x108c0u+at]);
                    else {
                        at=0;
                        while (at<(size_t)capacity*40u && candidate->ls[0x10d00u+at]==reference->ls[0x10d00u+at]) ++at;
                        if (at<(size_t)capacity*40u) fprintf(stderr,"native_update_diff=particle+%x %02x/%02x ",at,candidate->ls[0x10d00u+at],reference->ls[0x10d00u+at]);
                        else {
                            static const unsigned liveins[]={1,94,107,115,119,120,121,122};
                            unsigned i=0;
                            for (;i<sizeof liveins/sizeof liveins[0];++i)
                                if (memcmp(&candidate->gpr[liveins[i]],&reference->gpr[liveins[i]],sizeof candidate->gpr[0])) break;
                            if (i<sizeof liveins/sizeof liveins[0]) fprintf(stderr,"native_update_diff=gpr%u ",liveins[i]);
                            else fprintf(stderr,"native_update_diff=pc/status ");
                        }
                    }
                    fprintf(stderr,"mxcsr=%08x/%08x fields=%08x,%08x,%08x,%08x\n",candidate_mxcsr,reference_mxcsr,
                        ((uint32_t)candidate->ls[0x10c08]<<24)|((uint32_t)candidate->ls[0x10c09]<<16)|((uint32_t)candidate->ls[0x10c0a]<<8)|candidate->ls[0x10c0b],
                        ((uint32_t)candidate->ls[0x10cf8]<<24)|((uint32_t)candidate->ls[0x10cf9]<<16)|((uint32_t)candidate->ls[0x10cfa]<<8)|candidate->ls[0x10cfb],
                        ((uint32_t)candidate->ls[0x10bb0]<<24)|((uint32_t)candidate->ls[0x10bb1]<<16)|((uint32_t)candidate->ls[0x10bb2]<<8)|candidate->ls[0x10bb3],
                        ((uint32_t)candidate->ls[0x10bb4]<<24)|((uint32_t)candidate->ls[0x10bb5]<<16)|((uint32_t)candidate->ls[0x10bb6]<<8)|candidate->ls[0x10bb7]);
                }
            }
        } else {
            /* Verify rejection is non-mutating before diagnostic I/O can alter
             * process FP status or any other ambient state. */
            if (memcmp(candidate->gpr,ctx->gpr,sizeof ctx->gpr) ||
                memcmp(candidate->ls,ctx->ls,SPU_LS_SIZE) || candidate->status!=ctx->status ||
                _mm_getcsr()!=original_mxcsr) {
                atomic_fetch_add_explicit(&haze_job13_update_mismatches,1,memory_order_relaxed);
                atomic_fetch_add_explicit(&haze_job13_update_rejected_mutations,1,memory_order_relaxed);
            }
            const uint32_t reason=haze_job13_native_update_last_reject();
            if (reason<32u) atomic_fetch_add_explicit(&haze_job13_update_fallbacks[reason],1,memory_order_relaxed);
        }
    }
    free(candidate); free(reference);
    _mm_setcsr(original_mxcsr);
    spu_0031_at_00ECEC00_spu_func_00009A00_reference(ctx);
#else
    spu_0031_at_00ECEC00_spu_func_00009A00_reference(ctx);
#endif
}
/* END JOB13 NATIVE UPDATE WRAPPER */
'''


def patch(source, registration, mode=1, native_math=True, diagnostics=True):
    if registration.count(REGISTRATION) != 1:
        raise ValueError('image-32 registration changed')
    if mode not in (0, 1, 2):
        raise ValueError('mode must be reference, shadow, or execute')
    marker='void '+PREFIX+ENTRY+'(spu_context* ctx) {'
    if source.count(marker)!=1:
        raise ValueError('09A00 definition is missing or ambiguous')
    # Check the real block before fusion; this makes source order and the
    # original 09A00 entry an explicit audit dependency.
    import re
    regex=re.compile(re.escape(marker)+r'(.*?)\n\}',re.S)
    match=regex.search(source)
    if not match or hashlib.sha256(match.group(1).encode()).hexdigest()!=EXPECTED_ENTRY_BODY:
        raise ValueError('09A00 body changed; re-audit the loop entry')
    # Use the already-validated complete source as the composition boundary.
    # Native integer division is the committed baseline; the new record-loop
    # candidate must not silently fall back to the older translated DA08 path.
    fused=fuse_loop(source,registration,native_divide=True)
    fused_marker='void '+PREFIX+ENTRY+'(spu_context* ctx) {'
    if fused.count(fused_marker)!=1:
        raise ValueError('fused 09A00 entry is missing or ambiguous')
    renamed=fused_marker.replace('(spu_context* ctx)', '_reference(spu_context* ctx)')
    fused=fused.replace(fused_marker,renamed,1)
    include='#include "spu_helpers.h"'
    if fused.count(include)!=1:
        raise ValueError('generated helper include changed')
    mode_def='#define HAZE_JOB13_NATIVE_UPDATE_MODE '+str(mode)+'\n'
    fused=fused.replace(include,include+'\n#include "haze_job13_native_update.h"',1)
    if native_math:
        from patch_job13_native_math import patch_trusted as patch_math_trusted
        fused=patch_math_trusted(fused,execute_native=True,diagnostics=False)
    diagnostics_def='#define HAZE_JOB13_NATIVE_UPDATE_DIAGNOSTICS '+str(int(diagnostics))+'\n'
    return fused+mode_def+diagnostics_def+WRAPPER


def patch_baseline(source, registration):
    """Committed divide + DA08 math with the original, unfused record loop."""
    from patch_job13_native_divide import patch as patch_divide
    from patch_job13_native_math import patch_trusted as patch_math_trusted
    fused=patch_divide(source,registration)
    return patch_math_trusted(fused,execute_native=True,diagnostics=False)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    parser.add_argument('--registration',type=Path,required=True)
    parser.add_argument('--mode',choices=('reference','shadow','execute'),default='shadow')
    args=parser.parse_args()
    modes={'reference':0,'shadow':1,'execute':2}
    result=patch(args.source.read_text(encoding='utf-8'),
                 args.registration.read_text(encoding='utf-8'),modes[args.mode])
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8',newline='') as f: f.write(result)
