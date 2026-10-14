"""Bridge native pointer input at audited BLUS30094 GUI update boundaries."""
import hashlib

DIGESTS = {
    '002E0DD8': '8c9d9238fd4e2a93d5b900eedff763ee99f5b3601b846df57af2af9f18975ead',
    '0031B9C0': '33223cb9835f8f4d2cca2efc3ac0fa1cc48c17ae69e6c039c9856de69fd6253b',
    '002F69D8': '39d7411e78df3e06b757b6dfdba30c903b2b21cfbf873fae8b707b5f2992a842',
    '00301C18': '796f27b839e9ec717c38388f6f6052fd49c6e27db8ad92f2b00627095222c658',
}


def patch(source):
    for name, digest in DIGESTS.items():
        entry = f'void func_{name}(ppu_context* ctx) {{'
        if source.count(entry) != 1:
            raise ValueError(f'Expected one GUI function {name}')
        begin = source.index(entry)
        end = source.index('\nvoid ', begin + len(entry))
        body = source[begin:end]
        if hashlib.sha256(body.encode()).hexdigest() != digest:
            raise ValueError(f'GUI function {name} changed; audit before patching')
        if name == '00301C18':
            # The draw routine stores an already normalized thumb width at
            # +178. The pointer hit routine scales it by the widget width
            # again at 00301CFC, excluding the visible thumb's center.
            old = 'ppu_fmadd_core(ctx->fpr[4], ctx->fpr[0], ctx->fpr[12], 0, 0)'
            assert body.count(old) == 1
            body = body.replace(old, 'ppu_fadd(ctx->fpr[0], ctx->fpr[12])')
            source = source[:begin] + body + source[end:]
            continue
        if name == '002F69D8':
            # Scripted Confirm (event 1) excludes all point input at 002F6DA0.
            # r18 is the guest's topmost hit (00317618, flag 0x200), r25 the
            # current widget, r28 its event, and r21 the existing event context.
            # Keep the normal eligibility call and reuse the guest action path.
            start = body.index('loc_002F6D50:')
            stop = body.index('loc_002F6DF8:', start)
            section = body[start:stop]
            branch = '        if ((!((ctx->cr >> 0) & 2))) goto loc_002F6B40;'
            assert section.count(branch) == 2
            position = section.rindex(branch)
            replacement = '''        if ((!((ctx->cr >> 0) & 2))) {
            extern bool haze_menu_pointer_confirm(uint32_t,uint32_t,uint32_t);
            if (!haze_menu_pointer_confirm(static_cast<uint32_t>(ctx->gpr[29]),
                    static_cast<uint32_t>(ctx->gpr[25]),static_cast<uint32_t>(ctx->gpr[18])))
                goto loc_002F6B40;
            goto loc_002F74A8;
        }'''
            section = section[:position] + replacement + section[position+len(branch):]
            body = body[:start] + section + body[stop:]
            source = source[:begin] + body + source[end:]
            continue
        original = f'haze_original_{name}'
        body = body.replace(f'func_{name}', original, 1)
        if name == '002E0DD8':
            wrapper = f'''
{entry}
    extern void haze_menu_pointer_before(uint32_t);
    extern void haze_menu_pointer_after(uint32_t);
    const uint32_t input = static_cast<uint32_t>(ctx->gpr[3]);
    haze_menu_pointer_before(input);
    {original}(ctx);
    haze_menu_pointer_after(input);
}}
'''
        else:
            wrapper = f'''
{entry}
    extern void haze_menu_context(uint32_t);
    haze_menu_context(static_cast<uint32_t>(ctx->gpr[3]));
    {original}(ctx);
    DRAIN_TRAMPOLINE(ctx);
    // The manager's current-context slot is valid only during GUI dispatch.
    extern void haze_audio_guest_tick(ppu_context*);
    haze_audio_guest_tick(ctx);
    extern void haze_native_menu_tick(ppu_context*);
    haze_native_menu_tick(ctx);
}}
'''
        source = source[:begin] + body + wrapper + source[end:]
    return source
