"""Add bounded RAII timing scopes to Haze's main producer update.

Accepts the pinned generated function either before guest-pacer patching or
after that deterministic patch. Direct call sites are wrapped as C++ blocks so
their scopes end immediately after DRAIN_TRAMPOLINE returns, including across
branches and loops. High-frequency timer helpers are explicit exclusions.
"""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


FUNCTION = "void func_00680FA0(ppu_context* ctx) {"
DISPATCH_FUNCTION = "void func_006869F0(ppu_context* ctx) {"
DISPATCH_SHA256 = "1c64f130f99ef3f6817bbe2cd11e31c7616b89263bcc7e2e02a6ddfdc31499cd"
# Architectural return addresses verified against bctrl instructions in the ELF.
MAIN_INDIRECT_RETURNS = ("00681184", "006811DC")
DISPATCH_INDIRECT_RETURNS = ("00686AC8", "00686BE8", "00686C5C", "00686CFC")
ORIGINAL_SHA256 = "0f1c08565667f88ffe25988059165385f35cdbee26b5c6880220c25d0685af52"
GUEST_PACER_SHA256 = "56f434e9a85787e2471f2569106c05ee280af91f84d1b37efeecf10ffbf0ae1e"
INCLUDE = '#include "haze_producer_profile.h"\n'
EXCLUDED_CALLEES = {"00680D18", "0068EC78"}
CALL_RE = re.compile(
    r"(?m)^(?P<indent>[ \t]*)ctx->lr = 0x(?P<lr>[0-9A-Fa-f]+); "
    r"func_(?P<callee>[0-9A-Fa-f]+)\(ctx\); DRAIN_TRAMPOLINE\(ctx\);$"
)
FRAME_LINE = (
    "        haze_producer_profile::frame_scope haze_producer_frame("
    "ctx->thread_id, ctx->gpr[2], 0x00680FA0u, 0);\n"
)


def function_body(source: str, function: str = FUNCTION) -> str:
    start = source.find(function)
    if start < 0 or source.find(function, start + 1) >= 0:
        raise ValueError(f"expected exactly one {function} definition")
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise ValueError("unterminated func_00680FA0")


def call_sites(body: str) -> list[re.Match[str]]:
    return list(CALL_RE.finditer(body))


def _digest(body: str) -> str:
    return hashlib.sha256(body.encode("utf-8")).hexdigest()


def _call_scope(index: int, lr: str, callee: str, indent: str) -> str:
    inner = indent + "    "
    return (
        indent + "{\n"
        + inner + f"haze_producer_profile::call_scope haze_producer_call_{index}("
          f"haze_producer_frame, 0x{lr}u, 0x{callee}u);\n"
        + inner + f"ctx->lr = 0x{lr}; func_{callee}(ctx); DRAIN_TRAMPOLINE(ctx);\n"
        + indent + "}"
    )


def _indirect_scopes(body: str, returns: tuple[str, ...], parent: str) -> str:
    pattern = re.compile(r"(?m)^(?P<indent>[ \t]*)ps3_indirect_call\(ctx\); DRAIN_TRAMPOLINE\(ctx\);$")
    matches = list(pattern.finditer(body))
    if len(matches) != len(returns):
        raise ValueError("indirect call inventory changed; re-audit branch return addresses")
    for match, lr in reversed(list(zip(matches, returns))):
        indent = match.group("indent")
        inner = indent + "    "
        block = (indent + "{\n" + inner +
                 f"haze_producer_profile::call_scope haze_producer_indirect_{lr}("
                 f"{parent}0x{lr}u, static_cast<std::uint32_t>(ctx->ctr));\n" +
                 inner + "ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);\n" + indent + "}")
        body = body[:match.start()] + block + body[match.end():]
    return body


def _dispatch_scopes(source: str) -> str:
    body = function_body(source, DISPATCH_FUNCTION)
    if _digest(body + "\n") != DISPATCH_SHA256:
        raise ValueError("Haze callback dispatcher changed; audit before profiling")
    matches = call_sites(body)
    if len(matches) != 14:
        raise ValueError("callback dispatcher direct-call inventory changed")
    changed = body
    for match in reversed(matches):
        indent = match.group("indent")
        inner = indent + "    "
        lr, callee = match.group("lr"), match.group("callee")
        block = (indent + "{\n" + inner +
                 f"haze_producer_profile::call_scope haze_producer_nested_{lr}(0x{lr}u, 0x{callee}u);\n" +
                 inner + f"ctx->lr = 0x{lr}; func_{callee}(ctx); DRAIN_TRAMPOLINE(ctx);\n" + indent + "}")
        changed = changed[:match.start()] + block + changed[match.end():]
    changed = _indirect_scopes(changed, DISPATCH_INDIRECT_RETURNS, "")
    return source.replace(body, changed, 1)


def patch(source: str) -> str:
    body = function_body(source)
    # The original guard digest is defined over the source slice through the
    # newline before the next function, matching patch_guest_pacer.py.
    digest = _digest(body + "\n")
    if digest not in (ORIGINAL_SHA256, GUEST_PACER_SHA256):
        raise ValueError("Haze producer function changed; audit before profiling")
    sites = call_sites(body)
    if len(sites) != 74:
        raise ValueError(f"expected 74 direct call/trampoline sites, found {len(sites)}")
    excluded = [m for m in sites if m.group("callee").upper() in EXCLUDED_CALLEES]
    profiled = [m for m in sites if m.group("callee").upper() not in EXCLUDED_CALLEES]
    if len(excluded) != 15 or len(profiled) != 59:
        raise ValueError("main producer direct-call inventory changed; re-audit profile coverage")

    # Reverse-order substitutions keep match offsets stable.
    changed = body
    profile_index = len(profiled) - 1
    for match in reversed(profiled):
        index = profile_index
        profile_index -= 1
        replacement = _call_scope(index, match.group("lr"), match.group("callee"), match.group("indent"))
        changed = changed[:match.start()] + replacement + changed[match.end():]
    if profile_index != -1:
        raise AssertionError("profile call index accounting failed")
    changed = changed.replace(FUNCTION, FUNCTION + "\n" + FRAME_LINE.rstrip("\n"), 1)
    changed = _indirect_scopes(changed, MAIN_INDIRECT_RETURNS, "haze_producer_frame, ")

    if source.count(INCLUDE):
        if source.count(INCLUDE) != 1:
            raise ValueError("duplicate producer-profile header include")
        if source.index(INCLUDE) > source.index(FUNCTION):
            raise ValueError("producer-profile header appears after the target function")
        result = source.replace(INCLUDE, INCLUDE, 1)
    else:
        result = INCLUDE + source
    return _dispatch_scopes(result.replace(body, changed, 1))


def strip_profile(source: str) -> str:
    """Remove only generated instrumentation, for preservation tests."""
    if source.count(INCLUDE) != 1:
        raise ValueError("expected one producer-profile include")
    result = source.replace(INCLUDE, "", 1)
    indirect = re.compile(
        r"(?m)^(?P<indent>[ \t]*)\{\n"
        r"(?P=indent)    haze_producer_profile::call_scope haze_producer_indirect_(?P<lr>[0-9A-F]+)\("
        r"(?:haze_producer_frame, )?0x(?P=lr)u, static_cast<std::uint32_t>\(ctx->ctr\)\);\n"
        r"(?P=indent)    ps3_indirect_call\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n"
        r"(?P=indent)\}"
    )
    result, count = indirect.subn(lambda m: m.group("indent") + "ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);", result)
    if count != 6:
        raise ValueError(f"expected six indirect scopes, removed {count}")
    nested = re.compile(
        r"(?m)^(?P<indent>[ \t]*)\{\n"
        r"(?P=indent)    haze_producer_profile::call_scope haze_producer_nested_(?P<lr>[0-9A-F]+)\("
        r"0x(?P=lr)u, 0x(?P<callee>[0-9A-F]+)u\);\n"
        r"(?P=indent)    ctx->lr = 0x(?P=lr); func_(?P=callee)\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n"
        r"(?P=indent)\}"
    )
    result, count = nested.subn(lambda m: m.group("indent") + f"ctx->lr = 0x{m.group('lr')}; func_{m.group('callee')}(ctx); DRAIN_TRAMPOLINE(ctx);", result)
    if count != 14:
        raise ValueError(f"expected 14 dispatcher direct scopes, removed {count}")
    body = function_body(result)
    if body.count(FRAME_LINE) != 1:
        raise ValueError("missing or repeated producer frame scope")
    changed = body.replace(FRAME_LINE, "", 1)
    # Match each generated block, recover the guest line, and reject stray scopes.
    wrapper = re.compile(
        r"(?m)^(?P<indent>[ \t]*)\{\n"
        r"(?P=indent)    haze_producer_profile::call_scope haze_producer_call_\d+\("
        r"haze_producer_frame, 0x(?P<lr>[0-9A-Fa-f]+)u, 0x(?P<callee>[0-9A-Fa-f]+)u\);\n"
        r"(?P=indent)    ctx->lr = 0x(?P=lr); func_(?P=callee)\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n"
        r"(?P=indent)\}"
    )
    changed, count = wrapper.subn(
        lambda match: (match.group("indent") + f"ctx->lr = 0x{match.group('lr')}; "
                       f"func_{match.group('callee')}(ctx); DRAIN_TRAMPOLINE(ctx);"),
        changed,
    )
    if count != 59 or "haze_producer_call_" in changed:
        raise ValueError(f"expected 59 generated call scopes, removed {count}")
    result = result.replace(body, changed, 1)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="generated or build-local ppu_recomp_003.cpp")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = patch(args.source.read_text(encoding="cp1252"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding="cp1252")
    print("Producer profile scopes: 59 main direct calls, 14 dispatcher direct calls, six indirect sites; excluded helper callees:",
          ", ".join(sorted(EXCLUDED_CALLEES)))


if __name__ == "__main__":
    main()
