"""Extract the audited upload loop from a private Haze lift for boundary tests.

No generated game source is checked in. This does not install a runtime hook.
"""
import argparse
import hashlib
from pathlib import Path

FUNCTION_SHA256 = '16c315e0e8c3e43767b330e6cde60871b1a2ab984b0e2fa7a6292d4b9f4733d1'
LOOP_SHA256 = 'caa6fdaebfe305e19e1fc45ab82a5fffcf6d4726aa5492e9cda68ebf421bba46'

def extract(source):
    start = source.index('void func_0072C368(ppu_context* ctx) {')
    end = source.index('\nvoid func_', start + 1)
    body = source[start:end]
    if hashlib.sha256(body.encode()).hexdigest() != FUNCTION_SHA256:
        raise ValueError('Haze vertex upload function changed; re-audit before extraction')
    start = body.index('loc_0072C4F8:')
    end = body.index('        ctx->gpr[0] = (uint64_t)ppc_rlwinm', start)
    loop = body[start:end]
    if hashlib.sha256(loop.encode()).hexdigest() != LOOP_SHA256:
        raise ValueError('Haze vertex upload loop changed')
    return 'static void reference_vp_blocks(ppu_context* ctx) {\n' + loop + '}\n'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(extract(args.source.read_text()))
