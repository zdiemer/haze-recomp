/* Explicit semantic boundary for the native DA08 component. This retains
 * every channel/DMA/atomic comparison, final continuation, MXCSR, preserved
 * registers and all LS outside the audited dead math-frame writes. It does
 * NOT claim byte equality of caller-clobbered registers or dead stack. */
#include "spu_context.h"
static void compare_math_boundary(const spu_context*, const spu_context*, const uint8_t*, const uint8_t*);
#define HAZE_JOB13_COMPARE_FINAL compare_math_boundary
#define main original_replay_main
#include "replay_job13_capture.c"
#undef main

static void compare_math_boundary(const spu_context* ctx, const spu_context* initial,
                                  const uint8_t* expected_gpr, const uint8_t* expected_ls) {
    uint32_t outer_sp = initial->gpr[1]._u32[0];
    require(outer_sp >= 6512u + 336u && outer_sp < SPU_LS_SIZE, "invalid math ABI stack");
    uint32_t sp = outer_sp - 6512u;
    require(!(sp & 15u), "unaligned math ABI stack");
    equal_bytes(ctx->gpr, expected_gpr, 2 * sizeof(u128), "ABI link/stack");
    equal_bytes(ctx->gpr + 80, expected_gpr + 80 * sizeof(u128), 48 * sizeof(u128), "ABI preserved registers");
    /* Exempt exactly [-336,-320), [-304,0), [16,32) relative DA08 entrySP.
     * Both holes and ALL other memory remain strict. No IO buffer exemptions. */
    equal_bytes(ctx->ls, expected_ls, sp - 336u, "LS before math scratch");
    equal_bytes(ctx->ls + sp - 320u, expected_ls + sp - 320u, 16, "LS math scratch hole");
    equal_bytes(ctx->ls + sp, expected_ls + sp, 16, "LS outer backchain");
    equal_bytes(ctx->ls + sp + 32u, expected_ls + sp + 32u, SPU_LS_SIZE - sp - 32u, "LS above math linkage");
}

void haze_job13_native_math_report(void);
uint64_t haze_job13_native_math_failure_count(void);
int main(int argc, char** argv) {
    int result = original_replay_main(argc, argv);
    haze_job13_native_math_report();
    puts("BOUNDARY observable outputs + ABI; volatile registers and 336 dead stack bytes excluded");
    return result ? result : haze_job13_native_math_failure_count() ? 1 : 0;
}
