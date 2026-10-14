"""Invalidate host widget identities before Haze destroys a native GUI context."""
import hashlib

ENTRY = 'void func_00317300(ppu_context* ctx) {'
DIGEST = '5114afc6f9bcaaf1b171016781d05e227f3764e7917417108357c177debacd0b'
NOTIFY = '\n        extern void haze_native_menu_context_reset(uint32_t);\n        haze_native_menu_context_reset(static_cast<uint32_t>(ctx->gpr[3]));\n        extern void haze_menu_context_reset(uint32_t);\n        haze_menu_context_reset(static_cast<uint32_t>(ctx->gpr[3]));'


def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected one GUI root destruction function')
    start = source.index(ENTRY)
    end = source.index('\nvoid ', start + len(ENTRY))
    original = source[start:end]
    if hashlib.sha256(original.encode()).hexdigest() != DIGEST:
        raise ValueError('GUI destruction changed; audit native widget ownership')
    return source[:start] + original.replace(ENTRY, ENTRY + NOTIFY, 1) + source[end:]
