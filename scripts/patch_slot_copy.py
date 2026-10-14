"""Install the audited 0x1410 job-slot payload copy hook."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

FUNCTION = "void func_00682648(ppu_context* ctx) {"
FUNCTION_SHA256 = "cf058cabbf70f872c0dccf7f4b0547bbd1314ce4d5506199d8bc1a785e2b6fea"
MEMMOVE_FUNCTION = "void func_00C07078(ppu_context* ctx) {"
MEMMOVE_SHA256 = "24069f84158508e934e8e8933c88736acf32520f21283a27d4e1cd8fb582e226"
TRACE_CALLER_SHA256 = "a70c54ddaf9c969858f52225995c68005d57a80f908d68787d86c05d1ec2eab6"
CALLSITE = "ctx->lr = 0x00682798; func_00C07078(ctx); DRAIN_TRAMPOLINE(ctx);"
HOOK = (
    "ctx->lr = 0x00682798;\n"
    "        if (!haze_slot_copy_try(ctx)) func_00C07078(ctx);\n"
    "        DRAIN_TRAMPOLINE(ctx);"
)
CONTINUATION = (
    "\n        /* nop */;\n"
    "        ctx->gpr[4] = ppc_rldicl(ctx->gpr[23], 0, 32);\n"
    "        ctx->gpr[3] = ppc_rldicl(ctx->gpr[28], 0, 32);\n"
    "        ctx->gpr[5] = (int64_t)(int32_t)(0x10);"
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise ValueError(f"unterminated function: {signature}")


def verify_memmove(source: str) -> None:
    body = function_body(source, MEMMOVE_FUNCTION)
    if hashlib.sha256(body.encode()).hexdigest() != MEMMOVE_SHA256:
        raise ValueError("generated memmove changed; re-audit slot-copy semantics")


def verify_caller(source: str) -> str:
    body = function_body(source, FUNCTION)
    if hashlib.sha256((body + "\n").encode()).hexdigest() != FUNCTION_SHA256:
        raise ValueError("job-slot caller changed; re-audit ownership and continuation")
    if body.count(CALLSITE) != 1:
        raise ValueError("expected exactly one audited 0x00682798 memmove call")
    before, after = body.split(CALLSITE)
    if not after.startswith(CONTINUATION):
        raise ValueError("volatile-register continuation changed after slot copy")
    return body


def expected_traced_caller(body: str) -> str:
    """Recreate only job-trace's deterministic edits to this caller."""
    import patch_job_dispatch_trace as job_trace

    changed = body.replace("{\n", "{\n" + job_trace.entry("00682648"), 1)
    for call in job_trace.BASE_CALLS["00682648"].splitlines(keepends=True):
        if call in changed:
            changed = changed.replace(call, call + job_trace.BASE, 1)
    marker = "loc_00682740:\n"
    if changed.count(marker) != 1:
        raise ValueError("job-trace slot marker changed")
    changed = changed.replace(marker, marker + job_trace.SLOT, 1)
    marker = "loc_0068274C:\n"
    if changed.count(marker) != 1:
        raise ValueError("job-trace route marker changed")
    changed = changed.replace(marker, marker + job_trace.ROUTE, 1)
    if changed.count(job_trace.CALL) != 1:
        raise ValueError("job-trace dispatch call changed")
    changed = changed.replace(
        job_trace.CALL,
        job_trace.DISPATCH_BEGIN + job_trace.CALL + "\n" + job_trace.DISPATCH_END.rstrip("\n"),
        1,
    )
    return changed


def patch(source: str, memmove_source: str) -> str:
    verify_memmove(memmove_source)
    body = function_body(source, FUNCTION)
    digest = hashlib.sha256((body + "\n").encode()).hexdigest()
    if digest == FUNCTION_SHA256:
        pass
    elif digest == TRACE_CALLER_SHA256:
        # This digest is produced by expected_traced_caller() from the pinned
        # original function plus the job-trace transform constants.
        pass
    else:
        raise ValueError("job-slot caller changed; re-audit ownership and continuation")
    if body.count(CALLSITE) != 1:
        raise ValueError("expected exactly one audited 0x00682798 memmove call")
    if not source.startswith('#include "haze_job_trace.h"\n'):
        # Trace-free base callers are allowed in the earlier patch stage.
        if digest != FUNCTION_SHA256:
            raise ValueError("traced caller is missing its job-trace header")
    after_call = body.split(CALLSITE, 1)[1]
    if not after_call.startswith(CONTINUATION):
        raise ValueError("volatile-register continuation changed after slot copy")
    changed = body.replace(
        "{\n",
        "{\n        extern bool haze_slot_copy_try(ppu_context*);\n",
        1,
    ).replace(CALLSITE, HOOK, 1)
    return source.replace(body, changed, 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="generated ppu_recomp_003.cpp")
    parser.add_argument("--memmove-source", type=Path, required=True,
                        help="matching generated ppu_recomp_005.cpp")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = patch(args.source.read_text(), args.memmove_source.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)
    print("slot-copy caller SHA256:", FUNCTION_SHA256)
    print("original memmove SHA256:", MEMMOVE_SHA256)


if __name__ == "__main__":
    main()
