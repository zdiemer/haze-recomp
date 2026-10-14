"""Add a guarded title-string preparation call to a build-local PPU copy.

The original lookup and its register effects remain intact. The callback runs
before the loader compiles localized text into the game's layout/glyph objects.
"""
import hashlib

ENTRY = 'void func_005451A0(ppu_context* ctx) {'
DIGEST = 'd16b9cbaeaad7e4dcd990ebe4ac5cf5bbcc9acdbf38417f3b711f14db922f4cc'


def patch(source):
    begin = source.index(ENTRY)
    end = source.index('\nvoid ', begin + len(ENTRY))
    body = source[begin:end]
    if hashlib.sha256(body.encode()).hexdigest() != DIGEST:
        raise ValueError('Haze string lookup changed; audit before applying the title-prompt hook')
    hook = ('\n        extern void haze_title_prepare_text(uint32_t, uint32_t, uint32_t);'
            '\n        haze_title_prepare_text((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);')
    return source[:begin] + body.replace(ENTRY, ENTRY + hook, 1) + source[end:]
