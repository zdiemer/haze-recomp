"""Restore Haze's 0x29788 local switch in the build-local generated source.

The tracked/private generated lift is left untouched. The patch is hash-guarded
to the audited function and only adds a local CTR switch plus labels for the
nine already-emitted, otherwise unreachable case bodies.
"""
import hashlib


FUNCTION = 'void func_000296A0(ppu_context* ctx) {'
FUNCTION_SHA256 = '6d4b90264739d73d0a0a8bdb63568d30993ef7f99a815f4ad2fb63e3ec2a12cb'
CASES = (
    (0x0002984C, 0x00029860),
    (0x00029864, 0x0002987C),
    (0x00029880, 0x00029894),
    (0x00029898, 0x000298AC),
    (0x000298B0, 0x000298C0),
    (0x000298C4, 0x000298D8),
    (0x000298DC, 0x000298F0),
    (0x000298F4, 0x00029904),
    (0x00029908, 0x00029918),
)
FALLBACK = '        ps3_indirect_call(ctx); return;\n'
TABLE_END = '        /* TODO: .word 0x0000017C */;\n'
FIRST_CASE = ('        return;\n'
              '        ctx->fpr[1] = ctx->fpr[31];\n'
              '        ctx->gpr[31] = ppc_rldicl(ctx->gpr[28], 0, 32);\n')
COMMON_EPILOGUE = '        goto loc_0002973C;\n'


def _switch() -> str:
    lines = ['        switch ((uint32_t)ctx->ctr) {']
    lines.extend(f'        case 0x{target:08X}u: goto loc_{target:08X};'
                 for target, _ in CASES)
    lines.extend(('        default: ps3_indirect_call(ctx); return;',
                  '        }',
                  '        return;'))
    return '\n'.join(lines) + '\n'


SWITCH = _switch()
LABELS = tuple(f'loc_{target:08X}:\n' for target, _ in CASES)


def patch(source: str) -> str:
    if source.count(FUNCTION) != 1:
        raise ValueError('expected exactly one func_000296A0 definition')
    start = source.index(FUNCTION)
    end = source.find('\nvoid ', start + len(FUNCTION))
    if end < 0:
        raise ValueError('could not bound func_000296A0 body')
    body = source[start:end]
    if hashlib.sha256(body.encode()).hexdigest() != FUNCTION_SHA256:
        raise ValueError('func_000296A0 changed; re-audit before local dispatch patch')
    if (body.count(FALLBACK) != 1 or body.count(TABLE_END) != 1 or
            body.count(FIRST_CASE) != 1):
        raise ValueError('expected switch/fallback insertion anchors changed')

    # The first body follows the alternate path's return. Each later body
    # starts immediately after the previous case's shared-epilogue branch.
    body = body.replace(FIRST_CASE, FIRST_CASE.replace(
        '        ctx->fpr[1]', LABELS[0] + '        ctx->fpr[1]'), 1)
    for index, (target, return_pc) in enumerate(CASES[1:], start=1):
        marker = f'        ctx->lr = 0x{return_pc:08X};'
        if body.count(marker) != 1:
            raise ValueError(f'case 0x{target:08X} call anchor changed')
        call_pos = body.index(marker)
        separator_pos = body.rfind(COMMON_EPILOGUE, 0, call_pos)
        if separator_pos < 0:
            raise ValueError(f'case 0x{target:08X} shared-epilogue anchor missing')
        insert_pos = separator_pos + len(COMMON_EPILOGUE)
        body = body[:insert_pos] + LABELS[index] + body[insert_pos:]

    body = body.replace(FALLBACK, SWITCH, 1)
    patched = source[:start] + body + source[end:]
    if patched == source:
        raise ValueError('patch made no changes')
    return patched
