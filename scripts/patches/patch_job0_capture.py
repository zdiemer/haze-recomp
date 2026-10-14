"""Add bounded diagnostic input capture to three audited image-30 compute sites."""
import hashlib
from .prepare_job0_oracle import PREFIX, IMAGE_DIGEST


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST:
        raise ValueError('Job-0 image changed; re-audit fixture boundaries')
    source = source.replace('#include "spu_helpers.h"',
                            '#include "spu_helpers.h"\n#include "haze_job0_capture.h"', 1)
    for pc in ('00003408', '000036F0', '00003F48'):
        entry = f'void {PREFIX}{pc}(spu_context* ctx) {{\n'
        assert source.count(entry) == 1
        source = source.replace(entry, entry + '#if defined(HAZE_JOB_TRACE)\n'
            f'        if (ctx->image_id == 30) haze_job0_capture_input(0x{pc}, ctx->gpr, ctx->ls);\n'
            '#endif\n', 1)
    return source
