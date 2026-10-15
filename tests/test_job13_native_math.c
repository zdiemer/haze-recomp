/* Actual generated original DA08 is linked separately. No game operations or
 * assets are copied into this test. This tests the scalar semantic boundary,
 * not caller-clobbered scratch registers or a complete job replacement. */
#define main original_replay_main
#include "replay_job13_capture.c"
#undef main
#include "spu_helpers.h"
#include "haze_job13_native_math.h"

static uint64_t random_state = UINT64_C(0x7b019a3de0dc1285);
static uint64_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

int main(int argc, char** argv) {
    require(argc == 2, "expected valid private fixture");
    FILE* f = fopen(argv[1], "rb");
    require(f != NULL, "open fixture");
    read_exact(f, &header, sizeof header);
    require(header.valid == 1 && header.version == 2 && header.image_id == 32, "invalid fixture");
    spu_context* ctx = (spu_context*)_aligned_malloc(sizeof *ctx, 16);
    uint8_t* initial_ls = (uint8_t*)malloc(SPU_LS_SIZE);
    require(ctx && initial_ls, "allocate context");
    require(!fseek(f, sizeof header + 2 * HAZE_JOB13_CAPTURE_GPR_BYTES, SEEK_SET), "seek constants");
    read_exact(f, initial_ls, SPU_LS_SIZE);
    fclose(f);
    spu_0031_at_00ECEC00_spu_recomp_register();
    unsigned saved_csr = _mm_getcsr();
    unsigned mismatches = 0, flag_mismatches = 0;
    const uint64_t edges[] = {UINT64_C(0x0010000000000000), UINT64_C(0x7fefffffffffffff),
        UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x4010000000000000),
        UINT64_C(0x3fe0000000000000), UINT64_C(0x3fd0000000000000), UINT64_C(0x3fefffffffffffff)};
    for (unsigned trial = 0; trial < 20000; ++trial) {
        uint64_t bits = (next_random() & UINT64_C(0x000fffffffffffff)) |
                        ((1 + next_random() % 2046) << 52);
        if (trial < sizeof edges / sizeof *edges) bits = edges[trial];
        double input = haze_job13_double(bits), actual = -1;
        spu_context_init(ctx, 0);
        memcpy(ctx->ls, initial_ls, SPU_LS_SIZE);
        uint64_t upper = next_random();
        const uint64_t upper_edges[] = {0, UINT64_C(0x8000000000000000), 1,
            UINT64_C(0x000fffffffffffff), UINT64_C(0x7fefffffffffffff),
            UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
            UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8000000000001)};
        if (trial < 100) upper = upper_edges[trial % (sizeof upper_edges / sizeof *upper_edges)];
        for (unsigned byte = 0; byte < 8; ++byte)
            ctx->ls[0x3c000 - 72 + byte] = (uint8_t)(upper >> (56 - 8 * byte));
        ctx->image_id = 32; ctx->host_depth = 1;
        ctx->gpr[1] = spu_splat_u32(0x3c000);
        ctx->gpr[0] = spu_link(0xCE04);
        spu__dset(&ctx->gpr[3], 0, input);
        g_spu_trampoline_fn = NULL; steps = 0;
        _mm_setcsr(0x1f80);
        spu_0031_at_00ECEC00_spu_func_0000DA08(ctx);
        spu_drain_call(ctx, 0xCE04);
        double expected = spu__dget(ctx->gpr[3], 0);
        unsigned expected_csr = _mm_getcsr();
        _mm_setcsr(0x1f80);
        require(haze_job13_positive_sqrt(input, &actual), "normal input rejected");
        haze_job13_native_upper_sqrt_effects(bits, upper);
        unsigned actual_csr = _mm_getcsr();
        if (memcmp(&expected, &actual, sizeof actual)) {
            if (mismatches++ < 8)
                fprintf(stderr, "value mismatch input=%a expected=%a actual=%a\n", input, expected, actual);
        }
        if (expected_csr != actual_csr) {
            if (flag_mismatches++ < 8)
                fprintf(stderr, "flags mismatch input=%a upper=%016llx expected=%x actual=%x\n", input, (unsigned long long)upper, expected_csr, actual_csr);
        }
    }
    const uint64_t rejected[] = {0, UINT64_C(0x8000000000000000), 1,
        UINT64_C(0x000fffffffffffff), UINT64_C(0xbff0000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8000000000001)};
    for (unsigned i = 0; i < sizeof rejected / sizeof *rejected; ++i) {
        _mm_setcsr(0x1f80);
        double result = -123;
        require(!haze_job13_positive_sqrt(haze_job13_double(rejected[i]), &result), "unsupported input accepted");
        require(result == -123 && _mm_getcsr() == 0x1f80, "input rejection changed output or flags");
    }
    for (unsigned mode = 1; mode < 4; ++mode) {
        unsigned csr = 0x1f80 | (mode << 13);
        _mm_setcsr(csr);
        double result = -123;
        require(!haze_job13_positive_sqrt(2, &result), "unsupported rounding accepted");
        require(result == -123 && _mm_getcsr() == csr, "mode rejection changed output or flags");
    }
    for (unsigned exception = 0; exception < 6; ++exception) {
        unsigned csr = 0x1f80 & ~(1u << (7 + exception));
        _mm_setcsr(csr);
        double result = -123;
        require(!haze_job13_positive_sqrt(2, &result), "unmasked exceptions accepted");
        require(result == -123 && _mm_getcsr() == csr, "mask rejection changed output or flags");
    }
    _mm_setcsr(saved_csr);
    printf("DA08 scalar cases=20000 value_mismatches=%u flag_mismatches=%u\n", mismatches, flag_mismatches);
    _aligned_free(ctx); free(initial_ls);
    return mismatches || flag_mismatches ? 1 : 0;
}
