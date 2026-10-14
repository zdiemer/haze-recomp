"""Patch only BLUS30094's final frame wait in a build-local PPU source copy.

The translated update, rendering, clock sampling and frame bookkeeping remain
intact. The optional native selector bypasses only the legacy sleep/spin branch.
"""
import hashlib

ENTRY = 'void func_00680FA0(ppu_context* ctx) {'
DIGEST = '0f1c08565667f88ffe25988059165385f35cdbee26b5c6880220c25d0685af52'
SITE = 'loc_00681358:\n        ctx->lr = 0x0068135C; func_00680830(ctx); DRAIN_TRAMPOLINE(ctx);'


def patch(source):
    begin = source.index(ENTRY)
    end = source.index('\nvoid ', begin+len(ENTRY))
    body = source[begin:end]
    if hashlib.sha256(body.encode()).hexdigest() != DIGEST:
        raise ValueError('Haze main-loop pacing changed; audit before applying the native frame-pacer patch')
    body = body.replace(ENTRY, ENTRY+'\n        extern int haze_guest_pacer_bypass();',1)
    body = body.replace(SITE, SITE.replace('loc_00681358:',
        'loc_00681358:\n        if (haze_guest_pacer_bypass()) goto loc_006812DC;'),1)
    return source[:begin]+body+source[end:]
