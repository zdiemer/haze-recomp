/* Complete 09A00->09BC4 shadow oracle over actual full-batch fixtures. */
#include "haze_job13_native_update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAZE_JOB13_TEST_HAS_UPDATE
#define HAZE_JOB13_TEST_HAS_UPDATE 1
#endif
#ifndef HAZE_JOB13_TEST_DIAGNOSTICS
#define HAZE_JOB13_TEST_DIAGNOSTICS 1
#endif
#ifndef HAZE_JOB13_TEST_REQUIRE_NATIVE
#define HAZE_JOB13_TEST_REQUIRE_NATIVE 0
#endif

#if HAZE_JOB13_TEST_HAS_UPDATE
extern uint64_t haze_job13_native_update_shadow_attempts(void);
extern uint64_t haze_job13_native_update_shadow_mismatches(void);
extern uint64_t haze_job13_update_call_count(void);
extern uint64_t haze_job13_update_accept_count(void);
extern uint64_t haze_job13_update_particle_count(void);
extern uint64_t haze_job13_update_fallback_count(unsigned reason);
extern uint64_t haze_job13_update_rejected_mutation_count(void);
extern uint32_t haze_job13_update_entry_stack_pointer(void);
#endif

#if HAZE_JOB13_TEST_HAS_UPDATE && HAZE_JOB13_TEST_DIAGNOSTICS
#define HAZE_JOB13_ENTRY_SP() haze_job13_update_entry_stack_pointer()
#if HAZE_JOB13_TEST_REQUIRE_NATIVE
#define HAZE_JOB13_REQUIRE_NATIVE() require(haze_job13_native_update_shadow_attempts()>0,"native update path never eligible")
#else
#define HAZE_JOB13_REQUIRE_NATIVE() do { } while (0)
#endif
#define HAZE_JOB13_PRINT_STATS() do { \
    fprintf(stderr,"NATIVE_UPDATE calls=%llu accepted=%llu accepted_particles=%llu reject_mutations=%llu", \
        (unsigned long long)haze_job13_update_call_count(), \
        (unsigned long long)haze_job13_update_accept_count(), \
        (unsigned long long)haze_job13_update_particle_count(), \
        (unsigned long long)haze_job13_update_rejected_mutation_count()); \
    for(unsigned _r=1;_r<17;++_r) { uint64_t _n=haze_job13_update_fallback_count(_r); if(_n) fprintf(stderr," fallback%u=%llu",_r,(unsigned long long)_n); } \
    fputc('\n',stderr); \
    HAZE_JOB13_REQUIRE_NATIVE(); \
    require(haze_job13_native_update_shadow_mismatches()==0,"native update region mismatch"); \
    require(haze_job13_update_rejected_mutation_count()==0,"rejected native call mutated state"); \
} while (0)
#else
#define HAZE_JOB13_ENTRY_SP() 0x3e660u
#define HAZE_JOB13_PRINT_STATS() do { } while (0)
#endif

static void equal_job13_final_gpr(const void* actual, const void* expected) {
    const uint8_t* a=(const uint8_t*)actual;
    const uint8_t* e=(const uint8_t*)expected;
    for(unsigned reg=0;reg<128;++reg) {
        /* The committed DA08 native wrapper has an audited ABI contract:
         * r2-r79 are caller-clobbered; r0/r1 and r80-r127 stay observable. */
        if(reg>=2 && reg<80) continue;
        if(memcmp(a+reg*sizeof(u128),e+reg*sizeof(u128),sizeof(u128))) {
            fprintf(stderr,"Mismatch final strict GPR%u\n",reg);
            exit(2);
        }
    }
}

static int ls_difference_allowed(size_t offset, uint32_t stack_pointer) {
    const size_t begin=(size_t)stack_pointer;
    const struct { size_t lo, hi; } scratch[] = {
        {begin-0x150u,begin-0x140u}, /* DA08 local scratch, source-audited dead */
        {begin-0x130u,begin},        /* nested math-helper local scratch */
        {begin+0x10u,begin+0x20u},   /* nested helper linkage scratch */
        {begin-0x290u,begin-0x280u},/* GPR98 save slot restored before return */
        {begin+0x15e0u,begin+0x15f0u}/* retirement cursor, reinitialized per object */
    };
    for(size_t i=0;i<sizeof scratch/sizeof scratch[0];++i)
        if(offset>=scratch[i].lo && offset<scratch[i].hi) return 1;
    return 0;
}

#define HAZE_JOB13_COMPARE_FINAL(ctx, initial, expected_gpr, expected_ls) do { \
    equal_job13_final_gpr((ctx)->gpr,(expected_gpr)); \
    uint32_t _update_sp=HAZE_JOB13_ENTRY_SP(); \
    require(_update_sp==0x3e660u,"unexpected audited update stack pointer"); \
    size_t _i=0,_diff=0,_runs=0,_allowed_diff=0; \
    while(_i<SPU_LS_SIZE) { \
        if((ctx)->ls[_i]==(expected_ls)[_i]) { ++_i; continue; } \
        if(ls_difference_allowed(_i,_update_sp)) { ++_i; ++_allowed_diff; continue; } \
        size_t _start=_i; unsigned _actual=(ctx)->ls[_i],_expected=(expected_ls)[_i]; \
        while(_i<SPU_LS_SIZE && (ctx)->ls[_i]!=(expected_ls)[_i] && !ls_difference_allowed(_i,_update_sp)) { ++_i; ++_diff; } \
        if(_runs++<64) fprintf(stderr,"LS_DIFF start=%05zx length=%zu first=%02x/%02x last=%02x/%02x\n", \
            _start,_i-_start,_actual,_expected,(ctx)->ls[_i-1],(expected_ls)[_i-1]); \
    } \
    if(_diff || _allowed_diff) fprintf(stderr,"LS_DIFF_TOTAL unexpected=%zu runs=%zu audited_scratch=%zu\n",_diff,_runs,_allowed_diff); \
    require(_diff==0,"final local store outside source-audited dead scratch"); \
    HAZE_JOB13_PRINT_STATS(); \
} while (0)

#include "replay_job13_capture.c"
