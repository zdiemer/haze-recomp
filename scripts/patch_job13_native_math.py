"""Wrap guarded image32 DA08 for private native-math/reference diagnostics.

The default wrapper retains original computation and compares native results.
Explicit execute-native mode is an experimental semantic replacement and must
use the ABI/observable-output oracle, not claim full scratch-state equality.
Generated source and captures remain private.
"""
import argparse
from pathlib import Path

from patch_job13_capture import PREFIX, validate


WRAPPER = r'''
/* BEGIN JOB13 NATIVE MATH REFERENCE DIAGNOSTIC */
#include "haze_job13_native_math.h"
#include <inttypes.h>
#include <stdio.h>

enum haze_job13_math_reject {
    HZ_MATH_IMAGE_OR_CONTINUATION, HZ_MATH_INPUT, HZ_MATH_STACK,
    HZ_MATH_ROUNDING, HZ_MATH_EXCEPTION_MASKS,
    HZ_MATH_PRECISION_CLEAR, HZ_MATH_CONSTANTS, HZ_MATH_HELPER,
    HZ_MATH_REJECT_COUNT
};
/* Offline harness is single-threaded. No allocation, clocks or I/O in DA08. */
static struct {
    uint64_t total, eligible, rejected[HZ_MATH_REJECT_COUNT];
    uint64_t result_mismatches, flag_mismatches;
} haze_job13_math_diagnostic;
#if HAZE_JOB13_MATH_DIAGNOSTICS
#define HZ_MATH_COUNT(member) (++haze_job13_math_diagnostic.member)
#else
#define HZ_MATH_COUNT(member) ((void)0)
#endif

static int haze_job13_math_gate(const spu_context* ctx, unsigned csr) {
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
    const uint64_t input = ((uint64_t)ctx->gpr[3]._u32[0] << 32) |
                           ctx->gpr[3]._u32[1];
    const unsigned exponent = (unsigned)((input >> 52) & 2047u);
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if (ctx->image_id != 32 || (lr != 0xA180 && lr != 0xA450))
        return HZ_MATH_IMAGE_OR_CONTINUATION;
    if ((input >> 63) || !exponent || exponent == 2047u) return HZ_MATH_INPUT;
    /* DA08: 144 bytes; nested E968: 192 more. Include linkage at sp+16. */
    if ((sp & 15u) || sp < 336u || sp > SPU_LS_SIZE - 32u) return HZ_MATH_STACK;
    if (csr & 0x6000u) return HZ_MATH_ROUNDING;
    if ((csr & 0x1f80u) != 0x1f80u) return HZ_MATH_EXCEPTION_MASKS;
    /* Native DIVSD may set precision; original integer division does not. */
    if (!(csr & 0x20u)) return HZ_MATH_PRECISION_CLEAR;
    for (unsigned c = 0; c < sizeof expected / sizeof expected[0]; ++c) {
        const uint8_t* bytes = ctx->ls + expected[c].address;
        uint64_t preferred = 0, other = 0;
        for (unsigned b = 0; b < 8; ++b) {
            preferred = (preferred << 8) | bytes[b];
            other = (other << 8) | bytes[8u + b];
        }
        if (preferred != expected[c].preferred || other != expected[c].other)
            return HZ_MATH_CONSTANTS;
    }
    return -1;
}

void spu_0031_at_00ECEC00_spu_func_0000DA08(spu_context* ctx) {
    const unsigned original_csr = _mm_getcsr();
    const uint32_t return_pc = ctx->gpr[0]._u32[0];
    uint64_t native_bits = 0;
    unsigned native_csr = original_csr;
    int rejection = haze_job13_math_gate(ctx, original_csr);
    HZ_MATH_COUNT(total);
    if (rejection < 0) {
        const uint64_t input_bits = ((uint64_t)ctx->gpr[3]._u32[0] << 32) |
                                    ctx->gpr[3]._u32[1];
        double input, output;
        memcpy(&input, &input_bits, sizeof input);
        if (!haze_job13_positive_sqrt(input, &output)) {
            rejection = HZ_MATH_HELPER;
        } else {
            /* Preserve the unused working lane's actual host exception effects. */
            const uint32_t sp = ctx->gpr[1]._u32[0];
            uint64_t upper_bits = 0;
            for (unsigned i = 0; i < 8; ++i)
                upper_bits = (upper_bits << 8) | ctx->ls[sp - 72u + i];
            haze_job13_native_upper_sqrt_effects(input_bits, upper_bits);
            native_csr = _mm_getcsr();
            memcpy(&native_bits, &output, sizeof native_bits);
            HZ_MATH_COUNT(eligible);
#if HAZE_JOB13_EXECUTE_NATIVE_MATH
            /* Preferred result plus the original return cell's old upper lane.
             * All other registers and LS remain untouched; original fallback
             * owns unsupported cases. Callers rebuild volatile registers. */
            u128 result = spu_ls_read128(ctx, sp - 96u);
            result._u32[0] = (uint32_t)(native_bits >> 32);
            result._u32[1] = (uint32_t)native_bits;
            ctx->gpr[3] = result;
            SPU_RET(ctx);
#endif
        }
        /* Reference sees exactly the original FP state, even on rejection. */
        _mm_setcsr(original_csr);
    }
    if (rejection >= 0) HZ_MATH_COUNT(rejected[rejection]);
    spu_0031_at_00ECEC00_spu_func_0000DA08_reference(ctx);
    spu_drain_call(ctx, return_pc);
    const unsigned reference_csr = _mm_getcsr();
    if (rejection < 0) {
        const uint64_t reference_bits = ((uint64_t)ctx->gpr[3]._u32[0] << 32) |
                                        ctx->gpr[3]._u32[1];
        if (native_bits != reference_bits) HZ_MATH_COUNT(result_mismatches);
        if (native_csr != reference_csr) HZ_MATH_COUNT(flag_mismatches);
    }
    /* Registers, LS, flags and continuation remain the reference's result. */
}

uint64_t haze_job13_native_math_failure_count(void) {
    return haze_job13_math_diagnostic.result_mismatches +
           haze_job13_math_diagnostic.flag_mismatches;
}

void haze_job13_native_math_report(void) {
    static const char* names[HZ_MATH_REJECT_COUNT] = {
        "image_or_continuation", "input", "stack", "rounding",
        "exception_masks", "precision_clear", "constants", "helper"
    };
    printf("JOB13_NATIVE_MATH mode=%s reference_comparisons=%" PRIu64
           " total=%" PRIu64 " eligible=%" PRIu64
           " result_mismatches=%" PRIu64 " flag_mismatches=%" PRIu64,
           HAZE_JOB13_EXECUTE_NATIVE_MATH ? "native" : "shadow",
           HAZE_JOB13_EXECUTE_NATIVE_MATH ? UINT64_C(0) : haze_job13_math_diagnostic.eligible,
           haze_job13_math_diagnostic.total, haze_job13_math_diagnostic.eligible,
           haze_job13_math_diagnostic.result_mismatches,
           haze_job13_math_diagnostic.flag_mismatches);
    for (unsigned i = 0; i < HZ_MATH_REJECT_COUNT; ++i)
        printf(" rejected_%s=%" PRIu64, names[i], haze_job13_math_diagnostic.rejected[i]);
    putchar('\n');
}
/* END JOB13 NATIVE MATH REFERENCE DIAGNOSTIC */
'''


def patch_trusted(source, execute_native=False, diagnostics=False):
    """Apply only the exact DA08 rename/wrapper after an enclosing patcher has
    already validated the complete generated image source and registration.
    """
    definition = 'void ' + PREFIX + '0000DA08(spu_context* ctx) {'
    if source.count(definition) != 1:
        raise ValueError('DA08 definition is not unique')
    renamed = definition.replace('(spu_context* ctx)', '_reference(spu_context* ctx)')
    mode = '#define HAZE_JOB13_EXECUTE_NATIVE_MATH ' + str(int(execute_native)) + '\n'
    mode += '#define HAZE_JOB13_MATH_DIAGNOSTICS ' + str(int(diagnostics)) + '\n'
    return source.replace(definition, renamed, 1) + mode + WRAPPER


def patch(source, registration, execute_native=False, diagnostics=True):
    validate(source, registration)
    return patch_trusted(source, execute_native, diagnostics)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--registration', type=Path, required=True)
    parser.add_argument('--execute-native', action='store_true')
    args = parser.parse_args()
    transformed = patch(args.source.read_text(), args.registration.read_text(), args.execute_native)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x') as stream:
        stream.write(transformed)
