"""Keep characters on their highest-detail part mask when the setting is Full."""
import hashlib

# BLUS30094 01.00 character LOD component ("chr lod component", 'CLOD').
# 00245CE8 measures the character's camera distance into +20 and stores one
# of four 128-bit part-visibility masks in +38:
#   distance < +10 (7.015 in shipped characters)          -> +3C (77-84 parts)
#   +10 <= distance <= +14 (25.0)                          -> +4C (34-40 parts)
#   +14 < distance < +18 (999999)                          -> +5C (34-40 parts)
#   distance >= +18 (unmeasured / special)                 -> +6C (1 part)
# Full detail selects +3C wherever the game chose +4C or +5C. The +6C
# special case, the distance measurement and every other component field stay
# original. (The retail "LOD disabled" query 00243CE8 is NOT used: when it is
# forced on, characters receive the 1-part +6C mask and lose armor/helmets.)
ENTRY = 'void func_00245CE8(ppu_context* ctx) {'
DIGEST = '424c2bd0bbfa746cbc54dd9edaa6991065f7aebba5a05a1b0ef2a08e75bbb2d8'
SITES = {
    'ctx->gpr[0] = ctx->gpr[28] + (int64_t)(0x4C);':
        'ctx->gpr[0] = ctx->gpr[28] + (int64_t)(haze_character_lod_full() ? 0x3C : 0x4C);',
    'ctx->gpr[0] = ctx->gpr[28] + (int64_t)(0x5C);':
        'ctx->gpr[0] = ctx->gpr[28] + (int64_t)(haze_character_lod_full() ? 0x3C : 0x5C);',
}
DECLARATION = '\n        extern int haze_character_lod_full();'


def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected one character LOD selection')
    start = source.index(ENTRY)
    end = source.index('\nvoid ', start + len(ENTRY))
    body = source[start:end]
    if hashlib.sha256(body.encode('latin1')).hexdigest() != DIGEST:
        raise ValueError('Character LOD selection changed; audit before patching')
    for original, replacement in SITES.items():
        body = body.replace(original, replacement, 1)
    body = body.replace(ENTRY, ENTRY + DECLARATION, 1)
    return source[:start] + body + source[end:]
