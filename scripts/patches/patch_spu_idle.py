"""Add an opt-in host wait to Haze's exact empty-queue retry, in a build copy.

The generated/game-derived sources stay ignored and unchanged. Fail closed if
the audited caller changes: the queue call must still precede the same retry.
"""
import hashlib

ENTRY = 'void spu_0028_at_00EC7980_spu_func_00003378(spu_context* ctx) {'
DIGEST = 'e01f6d66d75b40b67e111fee425e55cb9b2009afa107da051a96d43a558379f7'
IMAGE_DIGEST = '8b5ca983a0b11d05e61763adb21742f68556ccf58e6610ebc7333d6db0ed5c1a'
RETRY = 'if (ctx->gpr[3]._u32[0] != 0) goto loc_00003378;'


def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST:
        raise ValueError('Haze SPU image changed; audit queue implementation before enabling the native wait patch')
    begin = source.index(ENTRY)
    end = source.index('\nvoid ', begin + len(ENTRY))
    body = source[begin:end]
    if hashlib.sha256(body.encode()).hexdigest() != DIGEST:
        raise ValueError('Haze SPU idle caller changed; audit before enabling the native wait patch')
    # The second retry follows a different operation and must remain untouched.
    body = body.replace(ENTRY, ENTRY + '\n        extern void haze_spu_idle_wait(spu_context*);', 1)
    body = body.replace(RETRY,
        'if (ctx->gpr[3]._u32[0] != 0) { haze_spu_idle_wait(ctx); goto loc_00003378; }', 1)
    return source[:begin] + body + source[end:]
