"""Extract private job-0 compute oracles and a register-local compiler experiment.

No extracted operations are checked in. This does not replace the whole job.
"""
import argparse
import hashlib
import re
from pathlib import Path

PREFIX = 'spu_0029_at_00EC9A80_spu_func_'
IMAGE_DIGEST = '6e580aa73e066ebc0b5736b14782b67ff8a9ed83af107629cb2342d7f2d8286b'


def body(source, address):
    start = source.index('void ' + PREFIX + address + '(spu_context* ctx) {')
    return source[start:source.index('\nvoid ', start + 5)]


def localize(block):
    """Keep temporaries outside the aliased context until the loop exits.

    Caller must exclude diagnostic callbacks and local-store/register aliasing.
    All written registers are exported, including scratch registers: no liveness
    or floating-point equivalence assumptions are made here.
    """
    if any(word in block for word in ('spu_wrch', 'spu_rdch', 'spu_indirect', 'spu_drain', 'spu_halt')):
        raise ValueError('Block contains externally observable operations')
    registers = sorted({int(v) for v in re.findall(r'ctx->gpr\[(\d+)\]', block)})
    written = sorted({int(v) for v in re.findall(r'ctx->gpr\[(\d+)\] =', block)})
    transformed = re.sub(r'ctx->gpr\[(\d+)\]', r'r\1', block)
    start = transformed.index('\n') + 1
    declarations = ''.join(f'    u128 r{r} = ctx->gpr[{r}];\n' for r in registers)
    transformed = transformed[:start] + declarations + transformed[start:]
    exports = ''.join(f'ctx->gpr[{r}] = r{r}; ' for r in written)
    return transformed.replace('return;', exports + 'return;')


def inline_arithmetic(block):
    result = block.replace('void job0_local_', '__attribute__((target("avx512f"))) void job0_inline_', 1)
    for old, new in (('spu_fa', 'haze_job0_add'), ('spu_fs', 'haze_job0_sub'),
                     ('spu_fm', 'haze_job0_mul'), ('spu_fma', 'haze_job0_madd'),
                     ('spu_fnms', 'haze_job0_nms')):
        result = result.replace(old + '(', new + '(')
    return result


def prepare(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST:
        raise ValueError('Job-0 image changed; re-audit extraction boundaries')
    text = '#include "spu_helpers.h"\n#include "haze_job0_arithmetic.h"\n#include "haze_job0_kernel.h"\n#include "haze_job0_capture.h"\n'
    for address in ('00003500', '00003B38', '000041D0'):
        text += f'static void {PREFIX}{address}(spu_context* ctx) {{ (void)ctx; }}\n'
    for address in ('00003408', '000036F0', '00003F48', '00004000'):
        text += f'void {PREFIX}{address}(spu_context* ctx);\n'
    for address in ('00003408', '000036F0', '00003F48', '00004000'):
        text += body(source, address) + '\n'
    localized = []
    for address in ('00003408', '000036F0'):
        localized.append(localize(body(source, address)).replace(PREFIX + address, 'job0_local_' + address))
    # Join the two pure-compute trampoline blocks into a compiler-visible loop.
    first, second = body(source, '00003F48'), body(source, '00004000')
    jump = '{ ctx->pc = 0x4000; g_spu_trampoline_fn = ' + PREFIX + '00004000; return; }'
    assert first.count(jump) == 1
    second = second[second.index('\n') + 1:second.rfind('}')]
    back = '{ ctx->pc = 0x3F48; g_spu_trampoline_fn = ' + PREFIX + '00003F48; return; }'
    assert second.count(back) == 1
    second = second.replace(back, 'goto job0_loop;')
    combined = first.replace(jump, second).replace('(spu_context* ctx) {\n', '(spu_context* ctx) {\njob0_loop:\n', 1)
    localized.append(localize(combined).replace(PREFIX + '00003F48', 'job0_local_00003F48'))
    for block in localized:
        text += block + '\n'
        text += inline_arithmetic(block) + '\n'
    from .patch_job0_kernel import patch
    integrated = patch(source)
    for pc in ('00003408', '000036F0'):
        text += body(integrated, pc).replace(PREFIX + pc, 'job0_integrated_' + pc, 1) + '\n'
    return text


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.source.read_text()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(prepare(source))
    print('Private oracle source SHA256:', hashlib.sha256(source.encode()).hexdigest())
