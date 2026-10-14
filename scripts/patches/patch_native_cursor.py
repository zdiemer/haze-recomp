"""Replace audited image-40 caller pairs with a PC cursor operation, opt-in.

Game-derived input remains local. Only native-kernel boundary code is public.
"""
import hashlib

PREFIX = 'spu_0039_at_00F1C100_spu_func_'
IMAGE_DIGEST = 'baa9580eca1701ea6301fcdf937dc266945aa998b79840360e7352ecefd823b0'
DIGESTS = {
    '00003450': 'fc8fc7b9549b83cd64599af46c87ea50ac55a612fe96e18dd06b50eb70580d53',
    '000047E0': '083d7ec5bde46bb86a0cc9bc0319914ffc68caacdb0b88c82dc5682d819f4c83',
    '00004900': 'f399efcd554d5aa0eddb1dfd85206ffa06dcf93b9fc5ce80077349bc6dc6339a',
}


def body(source, address):
    begin = source.index('void ' + PREFIX + address + '(spu_context* ctx) {')
    return source[begin:source.index('\nvoid ', begin + 5)]


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST or any(
            hashlib.sha256(body(source, addr).encode()).hexdigest() != digest
            for addr, digest in DIGESTS.items()):
        raise ValueError('Native cursor image/caller contract changed; re-audit before patching')
    for address, reg, ret, result_reg in [('000047E0', 88, '47F0', 41), ('00004900', 89, '4910', 55)]:
        old = body(source, address)
        start = old.index('\n') + 1
        end = old.index(f'        ctx->gpr[{result_reg}] =')
        calls = old[start:end]
        native = (f'        if (haze_native_cursor_enabled() && haze_native_cursor_pair(ctx,{reg},0x{ret})) {{\n'
                  '            haze_native_cursor_note();\n'
                  '        } else {\n' + calls + '        }\n')
        source = source.replace(old, old[:start] + native + old[end:], 1)
    return source.replace('#include "spu_helpers.h"',
                          '#include "spu_helpers.h"\n#include "haze_native_cursor_bridge.h"', 1)
