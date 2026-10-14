"""Allow skipping audited startup slide timers, retaining resource readiness.

Only build-local copies are changed. The original state transitions, texture
readiness call, and scene completion/cleanup remain the guest's responsibility.
"""
import hashlib

DIGESTS = {
    '00640440': 'a05743a4a0546cdf2094f329b8437ccefa7f8b34ef731630086b5485b0e74165',
    '006407C0': '6530506a963fcd1b30331f3948ac9685036b8bf3b3278485228349052fd1bcd6',
}


def patch(source):
    for name, digest in DIGESTS.items():
        entry = f'void func_{name}(ppu_context* ctx) {{'
        if source.count(entry) != 1:
            raise ValueError('Expected one startup splash function ' + name)
        begin = source.index(entry)
        end = source.index('\nvoid ', begin + len(entry))
        body = source[begin:end]
        if hashlib.sha256(body.encode()).hexdigest() != digest:
            raise ValueError('Startup splash function changed; audit before patching ' + name)
        if name == '00640440':
            anchor = '        vm_write32(ctx->gpr[28] + 0x10, ctx->gpr[3]);'
            body = body.replace(anchor, anchor +
                '\n        extern void haze_startup_texture_loaded(uint32_t,uint32_t);'
                '\n        haze_startup_texture_loaded((uint32_t)ctx->gpr[3],vm_read32(ctx->gpr[1]+0x70));', 1)
        else:
            body = body.replace(entry, entry +
                '\n        extern bool haze_startup_splash_tick(uint32_t);'
                '\n        extern void haze_startup_splash_end(uint32_t);'
                '\n        const uint32_t haze_scene=(uint32_t)ctx->gpr[3];'
                '\n        const bool haze_skip=haze_startup_splash_tick(haze_scene);', 1)
            # Two presentation-duration tests, after the unchanged texture-ready
            # branch, and one final fade-out test. No initialization is skipped.
            body = body.replace('if (((ctx->cr >> 0) & 4)) goto loc_00640808;',
                                'if (!haze_skip && ((ctx->cr >> 0) & 4)) goto loc_00640808;')
            body = body.replace('if (((ctx->cr >> 0) & 8)) goto loc_0064080C;',
                                'if (!haze_skip && ((ctx->cr >> 0) & 8)) goto loc_0064080C;', 1)
            body = body.replace('loc_00640944:',
                                'loc_00640944:\n        haze_startup_splash_end(haze_scene);', 1)
        source = source[:begin] + body + source[end:]
    return source
