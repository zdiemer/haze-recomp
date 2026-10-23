/* Replay one private captured image-32 batch through the patched build copy.
 * Retains every channel/DMA/atomic comparison,
 * final continuation, MXCSR, ABI-preserved registers and all LS outside the
 * audited dead square-root frame. Volatile registers and that dead stack are
 * excluded, as on experiment/native-job13. The native square root must run in
 * every batch; it subsumes most divisions, so the caller checks division
 * coverage across the fixture set. */
#include "spu_context.h"
static void compare_native_boundary(const spu_context*, const spu_context*, const uint8_t*, const uint8_t*);
#define HAZE_JOB13_COMPARE_FINAL compare_native_boundary
#define main original_replay_main
#include "replay_job13_capture.c"
#undef main

static unsigned native_counts[2];
void haze_job13_native_note(unsigned operation) { if (operation < 2) ++native_counts[operation]; }

static void compare_native_boundary(const spu_context* ctx, const spu_context* initial,
                                    const uint8_t* expected_gpr, const uint8_t* expected_ls) {
    uint32_t outer_sp = initial->gpr[1]._u32[0];
    require(outer_sp >= 6512u + 336u && outer_sp < SPU_LS_SIZE, "invalid math ABI stack");
    uint32_t sp = outer_sp - 6512u;
    require(!(sp & 15u), "unaligned math ABI stack");
    equal_bytes(ctx->gpr, expected_gpr, 2 * sizeof(u128), "ABI link/stack");
    equal_bytes(ctx->gpr + 80, expected_gpr + 80 * sizeof(u128), 48 * sizeof(u128), "ABI preserved registers");
    /* Exempt exactly [-336,-320), [-304,0), [16,32) relative DA08 entry SP. */
    equal_bytes(ctx->ls, expected_ls, sp - 336u, "LS before math scratch");
    equal_bytes(ctx->ls + sp - 320u, expected_ls + sp - 320u, 16, "LS math scratch hole");
    equal_bytes(ctx->ls + sp, expected_ls + sp, 16, "LS outer backchain");
    equal_bytes(ctx->ls + sp + 32u, expected_ls + sp + 32u, SPU_LS_SIZE - sp - 32u, "LS above math linkage");
}

int main(int argc, char** argv) {
    int result = original_replay_main(argc, argv);
    printf("NATIVE divide=%u sqrt=%u\n", native_counts[0], native_counts[1]);
    if (result) return result;
    return native_counts[1] ? 0 : 3;
}
