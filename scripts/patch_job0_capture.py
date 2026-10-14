"""Add bounded diagnostic input capture to three audited image-30 compute sites."""
import argparse
import hashlib
from pathlib import Path
from prepare_job0_oracle import PREFIX, IMAGE_DIGEST


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
