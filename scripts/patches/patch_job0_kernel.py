"""Compile audited job-0 loops with host kernels and a guarded fallback.

The entire job wrapper, DMA, completion and unsupported variants stay original.
0x36F0 first tries the generated host-SIMD kernel (job0_simd.py), then the
inlined-arithmetic copy. All emitted game-derived operations stay in the
build directory.
"""
import hashlib
from .prepare_job0_oracle import PREFIX, IMAGE_DIGEST, body, localize, inline_arithmetic
from .patch_job0_capture import patch as capture_patch
from .job0_simd import kernel

SIMD_SITES = ('000036F0',)


def simd_kernels(source):
    return ''.join(kernel(body(source, pc), 'job0_simd_' + pc, 'job0_step_' + pc)[0]
                   for pc in SIMD_SITES)


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST:
        raise ValueError('Job-0 image changed; re-audit native compute boundaries')
    result = capture_patch(source)
    include = '#include "haze_job0_capture.h"'
    result = result.replace(include, include + '\n#include "haze_job0_kernel.h"\n'
        '#if defined(SPU_FLOAT_NATIVE_HOST)\n#include "haze_job0_arithmetic.h"\n'
        '#include "haze_job0_simd.h"\n#endif', 1)
    simd = simd_kernels(source)
    # Only the two sites represented by live jungle fixtures are enabled.
    for pc in ('00003408', '000036F0'):
        candidate = inline_arithmetic(localize(body(source, pc)).replace(PREFIX + pc, 'job0_local_' + pc))
        candidate = candidate.replace('__attribute__((target', 'static __attribute__((target', 1)
        if pc in SIMD_SITES:
            candidate += '\n' + simd
        old = body(result, pc)
        capture_end = old.index('#endif\n') + len('#endif\n')
        fast = (f'            if (haze_job0_simd_supported() && job0_simd_{pc}(ctx)) return;\n'
                if pc in SIMD_SITES else '')
        new = old[:capture_end] + '#if defined(SPU_FLOAT_NATIVE_HOST)\n' + (
            f'        if (haze_job0_kernel_accept(ctx, 0x{pc})) {{\n'
            f'            haze_job0_kernel_note(0x{pc});\n' + fast +
            f'            job0_inline_{pc}(ctx); return;\n'
            '        }\n#endif\n') + old[capture_end:]
        result = result.replace(old, '#if defined(SPU_FLOAT_NATIVE_HOST)\n' + candidate + '#endif\n\n' + new, 1)
    return result
