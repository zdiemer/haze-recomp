"""Insert native arithmetic into Haze's audited image-32 effects batch.

Three guarded operations, validated against complete captured combat
batches on experiment/native-job13 (#142):

* EB10: the first 60 restoring-division steps run on host integers; the final
  original step still builds the rounding state and scratch registers.
* DA08: the effects square root runs its exact polynomial and three Newton
  divisions in host scalar SSE, preserving the unused lane's FP status.
* 09A00: the complete particle loop (age/retire, integrate, drag, forces,
  bounds) runs on typed host data and commits the object and particle ranges
  once; any unsupported object, particle or helper case runs the original loop.

All always run, and fall back to the unchanged translation whenever inputs or
FP modes are unsupported, debug LS watches are armed, or the image constants
differ. A changed image fails closed. Never modify or publish the generated input.
"""
import hashlib
import re

PREFIX = 'spu_0031_at_00ECEC00_spu_func_'
IMAGE_DIGEST = '98222b50bfa6fa2015ee9b9e1e592eae11c166b05f57c46136ce69d5c925a694'
INCLUDE = '#include "spu_helpers.h"'
HEADER = INCLUDE + '\n#include "haze_job13_native.h"'
DIVIDE_ENTRY = 'void ' + PREFIX + '0000EB10(spu_context* ctx) {'
DIVIDE_ANCHOR = '        { ctx->pc = 0xEB38; g_spu_trampoline_fn = ' + PREFIX + '0000EB38; return; }'
DIVIDE_INSERT = ('        if (haze_job13_native_ready(ctx) && haze_job13_native_divide_prefix(ctx)) {\n'
                 '            haze_job13_native_note(HAZE_JOB13_NATIVE_DIVIDE);\n'
                 '            ctx->pc = 0xEB24; g_spu_trampoline_fn = ' + PREFIX + '0000EB24; return;\n'
                 '        }\n')
SQRT_ENTRY = 'void ' + PREFIX + '0000DA08(spu_context* ctx) {'
SQRT_REFERENCE = 'void ' + PREFIX + '0000DA08_reference(spu_context* ctx) {'
WRAPPER = r'''
/* BEGIN HAZE JOB13 NATIVE SQRT */
/* Returns nonzero when DA08's audited native boundary applies. */
static int haze_job13_sqrt_supported(const spu_context* ctx, unsigned csr) {
    static const struct { uint32_t address; uint64_t preferred, other; } expected[] = {
        {0xf3d0u, UINT64_C(0xbfb98a86d71f3626), 0},
        {0xf3e0u, UINT64_C(0x3fe6bb2fec56d5d0), 0},
        {0xf3f0u, UINT64_C(0x3fd8be0ded288ce7), 0},
        {0xf400u, UINT64_C(0x3fd0000000000000), 0},
        {0xf410u, UINT64_C(0x3fe0000000000000), 0},
        /* Software division carry/borrow shuffles and result packing masks. */
        {0xf270u, UINT64_C(0x0405060780808080), UINT64_C(0x0c0d0e0f80808080)},
        {0xf2e0u, UINT64_C(0x04050607c0c0c0c0), UINT64_C(0x0c0d0e0fc0c0c0c0)},
        {0xf580u, UINT64_C(0x000fffffffffffff), 0},
        {0xf590u, UINT64_C(0x0fffffffffffffff), 0},
        {0xf5a0u, UINT64_C(0x00000000000007ff), 0},
        {0xf5b0u, UINT64_C(0x1fffffffffffffff), 0}
    };
    const uint32_t lr = ctx->gpr[0]._u32[0];
    const uint64_t input = ((uint64_t)ctx->gpr[3]._u32[0] << 32) | ctx->gpr[3]._u32[1];
    const unsigned exponent = (unsigned)((input >> 52) & 2047u);
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if (lr != 0xA180 && lr != 0xA450) return 0;
    if ((input >> 63) || !exponent || exponent == 2047u) return 0;
    /* DA08: 144 bytes; nested E968: 192 more. Include linkage at sp+16. */
    if ((sp & 15u) || sp < 336u || sp > SPU_LS_SIZE - 32u) return 0;
    if (csr & 0x6000u) return 0;
    if ((csr & 0x1f80u) != 0x1f80u) return 0;
    /* Native DIVSD may set precision; original integer division does not. */
    if (!(csr & 0x20u)) return 0;
    for (unsigned c = 0; c < sizeof expected / sizeof expected[0]; ++c) {
        const uint8_t* bytes = ctx->ls + expected[c].address;
        uint64_t preferred = 0, other = 0;
        for (unsigned b = 0; b < 8; ++b) {
            preferred = (preferred << 8) | bytes[b];
            other = (other << 8) | bytes[8u + b];
        }
        if (preferred != expected[c].preferred || other != expected[c].other) return 0;
    }
    return 1;
}

void spu_0031_at_00ECEC00_spu_func_0000DA08(spu_context* ctx) {
    const uint32_t return_pc = ctx->gpr[0]._u32[0];
    if (haze_job13_native_ready(ctx)) {
        const unsigned original_csr = _mm_getcsr();
        if (haze_job13_sqrt_supported(ctx, original_csr)) {
            const uint64_t input_bits = ((uint64_t)ctx->gpr[3]._u32[0] << 32) | ctx->gpr[3]._u32[1];
            double input, output;
            memcpy(&input, &input_bits, sizeof input);
            if (haze_job13_positive_sqrt(input, &output)) {
                /* Preserve the unused working lane's actual host exception effects. */
                const uint32_t sp = ctx->gpr[1]._u32[0];
                uint64_t upper_bits = 0, native_bits;
                for (unsigned i = 0; i < 8; ++i)
                    upper_bits = (upper_bits << 8) | ctx->ls[sp - 72u + i];
                haze_job13_native_upper_sqrt_effects(input_bits, upper_bits);
                memcpy(&native_bits, &output, sizeof native_bits);
                /* Preferred result plus the original return cell's old upper
                 * lane. All other registers and LS remain untouched; callers
                 * rebuild volatile registers. */
                u128 result = spu_ls_read128(ctx, sp - 96u);
                result._u32[0] = (uint32_t)(native_bits >> 32);
                result._u32[1] = (uint32_t)native_bits;
                ctx->gpr[3] = result;
                haze_job13_native_note(HAZE_JOB13_NATIVE_SQRT);
                SPU_RET(ctx);
            }
        }
        /* The original sees exactly its entry FP state. */
        _mm_setcsr(original_csr);
    }
    spu_0031_at_00ECEC00_spu_func_0000DA08_reference(ctx);
    spu_drain_call(ctx, return_pc);
}
/* END HAZE JOB13 NATIVE SQRT */

/* BEGIN HAZE JOB13 NATIVE PARTICLE UPDATE */
void spu_0031_at_00ECEC00_spu_func_00009A00(spu_context* ctx) {
    if (haze_job13_native_ready(ctx) && haze_job13_native_update_try(ctx)) {
        haze_job13_native_note(HAZE_JOB13_NATIVE_UPDATE);
        ctx->pc = 0x9BC4;
        g_spu_trampoline_fn = spu_0031_at_00ECEC00_spu_func_00009BC4;
        return;
    }
    spu_0031_at_00ECEC00_spu_func_00009A00_reference(ctx);
}
/* END HAZE JOB13 NATIVE PARTICLE UPDATE */
'''
UPDATE_ENTRY = 'void ' + PREFIX + '00009A00(spu_context* ctx) {'
UPDATE_REFERENCE = 'void ' + PREFIX + '00009A00_reference(spu_context* ctx) {'
UPDATE_EXIT = 'void ' + PREFIX + '00009BC4(spu_context* ctx) {'
# Digest of the 09A00 loop-entry block body, between its brace and the
# closing line. The native particle update owns 09A00 -> 09BC4.
UPDATE_ENTRY_BODY = '9f88830479ba5d357eb8a64b997410da95a231cd8a6c91ba1d4d913a9b9cca08'


def function_body(source, entry):
    begin = source.index(entry)
    return source[begin:source.index('\n}\n', begin)]


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST:
        raise ValueError('Haze image-32 source changed; re-audit the native effects arithmetic')
    if (source.count(INCLUDE) != 1 or source.count(DIVIDE_ENTRY) != 1 or source.count(SQRT_ENTRY) != 1 or
            source.count(UPDATE_ENTRY) != 1 or source.count(UPDATE_EXIT) != 1):
        raise ValueError('Image-32 native arithmetic boundaries are not unique')
    update = re.search(re.escape(UPDATE_ENTRY) + r'(.*?)\n\}', source, re.S)
    if hashlib.sha256(update.group(1).encode()).hexdigest() != UPDATE_ENTRY_BODY:
        raise ValueError('Image-32 particle loop entry changed')
    divide = function_body(source, DIVIDE_ENTRY)
    if divide.count(DIVIDE_ANCHOR) != 1:
        raise ValueError('Image-32 division entry changed')
    source = source.replace(divide, divide.replace(DIVIDE_ANCHOR, DIVIDE_INSERT + DIVIDE_ANCHOR), 1)
    source = source.replace(SQRT_ENTRY, SQRT_REFERENCE, 1).replace(UPDATE_ENTRY, UPDATE_REFERENCE, 1)
    return source.replace(INCLUDE, HEADER, 1) + WRAPPER
