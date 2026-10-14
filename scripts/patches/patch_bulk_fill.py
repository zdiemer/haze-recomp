"""Install the audited native fill at the job-slot 64-byte loop boundary."""
from __future__ import annotations

import hashlib

FUNCTION = "void func_00C11EA8(ppu_context* ctx) {"
CALLER_FUNCTION = "void func_00682648(ppu_context* ctx) {"
FUNCTION_SHA256 = "1c20cd8a0069f2bb760ebca301c6edb369dbf87cc7b53b8d2ef77df0a23f7a42"
CALLER_SHA256 = "cf058cabbf70f872c0dccf7f4b0547bbd1314ce4d5506199d8bc1a785e2b6fea"
CALLSITE = "ctx->lr = 0x00682774; func_00C11EA8(ctx); DRAIN_TRAMPOLINE(ctx);"
LOOP_ENTRY = (
    "        if (((ctx->cr >> 0) & 4)) goto loc_00C11F48;\n"
    "loc_00C11F08:"
)
HOOK = (
    "        if (ctx->lr == 0x00682774 && haze_bulk_fill_try(ctx)) "
    "goto loc_00C11F48;\n"
)


def _body(source: str, signature: str) -> str:
    start = source.index(signature)
    end = source.index("\nvoid ", start + len(signature))
    return source[start:end]


def verify_caller(source: str) -> None:
    body = _body(source, CALLER_FUNCTION)
    if hashlib.sha256(body.encode()).hexdigest() != CALLER_SHA256:
        raise ValueError("job-slot caller changed; re-audit ownership and publication")
    if body.count(CALLSITE) != 1:
        raise ValueError("audited memset call site is missing or duplicated")
    before, after = body.split(CALLSITE)
    expected_following = (
        "\n        /* nop */;\n"
        "        ctx->gpr[0] = vm_read32(ctx->gpr[1] + 0x70);"
    )
    if not after.startswith(expected_following):
        raise ValueError("slot initialization continuation changed")


def patch(source: str, caller_source: str) -> str:
    verify_caller(caller_source)
    body = _body(source, FUNCTION)
    if hashlib.sha256(body.encode()).hexdigest() != FUNCTION_SHA256:
        raise ValueError("bulk-fill function changed; re-audit loop semantics")
    if body.count(LOOP_ENTRY) != 1:
        raise ValueError("expected single initial branch / 64-byte loop entry")
    changed = body.replace(
        "{\n",
        "{\n        extern bool haze_bulk_fill_try(ppu_context*);\n",
        1,
    )
    changed = changed.replace(LOOP_ENTRY, LOOP_ENTRY.replace("loc_00C11F08:", HOOK + "loc_00C11F08:"), 1)
    return source.replace(body, changed, 1)
