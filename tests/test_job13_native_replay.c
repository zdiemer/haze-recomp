/* Replay one private captured image-32 batch through the patched build copy.
 * Retains every channel/DMA/atomic comparison, final continuation, MXCSR,
 * ABI-preserved registers (r0, r1, r80-r127) and all LS outside five audited
 * dead stack slots, as on experiment/native-job13. The native square root and
 * particle update must run in every batch; the square root subsumes most
 * divisions, so the caller checks division coverage across the fixture set. */
#include "spu_context.h"
static void compare_native_boundary(const spu_context*, const spu_context*, const uint8_t*, const uint8_t*);
#define HAZE_JOB13_COMPARE_FINAL compare_native_boundary
#define main original_replay_main
#include "replay_job13_capture.c"
#undef main

static unsigned native_counts[3];
void haze_job13_native_note(unsigned operation) { if (operation < 3) ++native_counts[operation]; }

/* Offsets relative to the particle loop / DA08 entry SP (outer SP - 0x1970). */
static int dead_scratch(uint32_t offset, uint32_t sp) {
    static const struct { int32_t lo, hi; } slots[] = {
        {-0x150, -0x140},  /* DA08 local scratch */
        {-0x130, 0},       /* nested math-helper local scratch */
        {0x10, 0x20},      /* nested helper linkage scratch */
        {-0x290, -0x280},  /* GPR98 save slot restored before return */
        {0x15E0, 0x15F0},  /* retirement cursor, reinitialized per object */
    };
    const int64_t relative = (int64_t)offset - sp;
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; ++i)
        if (relative >= slots[i].lo && relative < slots[i].hi) return 1;
    return 0;
}

static void compare_native_boundary(const spu_context* ctx, const spu_context* initial,
                                    const uint8_t* expected_gpr, const uint8_t* expected_ls) {
    uint32_t outer_sp = initial->gpr[1]._u32[0];
    require(outer_sp >= 0x1970u + 0x290u && outer_sp < SPU_LS_SIZE, "invalid update ABI stack");
    uint32_t sp = outer_sp - 0x1970u;
    require(!(sp & 15u), "unaligned update ABI stack");
    equal_bytes(ctx->gpr, expected_gpr, 2 * sizeof(u128), "ABI link/stack");
    equal_bytes(ctx->gpr + 80, expected_gpr + 80 * sizeof(u128), 48 * sizeof(u128), "ABI preserved registers");
    for (uint32_t i = 0; i < SPU_LS_SIZE; ++i)
        if (ctx->ls[i] != expected_ls[i] && !dead_scratch(i, sp)) {
            fprintf(stderr, "Mismatch final local store byte=%05x actual=%02x expected=%02x\n",
                    i, ctx->ls[i], expected_ls[i]);
            fail("final local store outside audited dead scratch");
        }
}

int main(int argc, char** argv) {
    int result = original_replay_main(argc, argv);
    printf("NATIVE divide=%u sqrt=%u update=%u\n", native_counts[0], native_counts[1], native_counts[2]);
    if (result) return result;
    return native_counts[1] && native_counts[2] ? 0 : 3;
}
