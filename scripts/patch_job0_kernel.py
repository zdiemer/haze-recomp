"""Compile audited job-0 loops with inlined arithmetic and a guarded fallback.

The entire job wrapper, DMA, completion and unsupported variants stay original.
All emitted game-derived operations stay in the build directory.
"""
import argparse
import hashlib
from pathlib import Path
from prepare_job0_oracle import PREFIX, IMAGE_DIGEST, body, localize, inline_arithmetic
from patch_job0_capture import patch as capture_patch


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST:
        raise ValueError('Job-0 image changed; re-audit native compute boundaries')
    result = capture_patch(source)
    include = '#include "haze_job0_capture.h"'
    result = result.replace(include, include + '\n#include "haze_job0_kernel.h"\n'
        '#if defined(SPU_FLOAT_NATIVE_HOST)\n#include "haze_job0_arithmetic.h"\n#endif', 1)
    # Only the two sites represented by live jungle fixtures are enabled.
    for pc in ('00003408', '000036F0'):
        candidate = inline_arithmetic(localize(body(source, pc)).replace(PREFIX + pc, 'job0_local_' + pc))
        candidate = candidate.replace('__attribute__((target', 'static __attribute__((target', 1)
        old = body(result, pc)
        capture_end = old.index('#endif\n') + len('#endif\n')
        new = old[:capture_end] + '#if defined(SPU_FLOAT_NATIVE_HOST)\n' + (
            f'        if (haze_job0_kernel_accept(ctx, 0x{pc})) {{\n'
            f'            haze_job0_kernel_note(0x{pc});\n'
            f'            job0_inline_{pc}(ctx); return;\n'
            '        }\n#endif\n') + old[capture_end:]
        result = result.replace(old, '#if defined(SPU_FLOAT_NATIVE_HOST)\n' + candidate + '#endif\n\n' + new, 1)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--registration', type=Path, required=True)
    args = parser.parse_args()
    if 'spu_begin_image(30); spu_0029_at_00EC9A80_spu_recomp_register();' not in args.registration.read_text():
        raise ValueError('Job-0 runtime registration changed')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(patch(args.source.read_text()))
