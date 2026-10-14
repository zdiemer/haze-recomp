"""Extract hash-pinned baseline memmove and its audited caller continuation."""
from __future__ import annotations

import hashlib
from pathlib import Path

from patch_slot_copy import (
    CALLSITE,
    FUNCTION,
    MEMMOVE_FUNCTION,
    MEMMOVE_SHA256,
    function_body,
    patch,
    verify_caller,
)

ROTATE_BEGIN = "/* Rotate helpers */"
ROTATE_END = "static inline int64_t ppc_sraw"
ROTATE_SHA256 = "ef126c77ecdd331a0577925f7b8bfb695ba9b9480251723a581916744a23675c"
CONTINUATION_END = "ctx->lr = 0x006827B4; func_00C0A6B0(ctx); DRAIN_TRAMPOLINE(ctx);"


def _rename(body: str, signature: str, name: str) -> str:
    return body.replace(signature, f"void {name}(ppu_context* ctx) {{", 1)


def extract(generated_dir: Path) -> tuple[str, str, str, str, str, str]:
    caller_path = generated_dir / "ppu_recomp_003.cpp"
    move_path = generated_dir / "ppu_recomp_005.cpp"
    helper_path = generated_dir / "ppu_recomp_006.cpp"
    caller_source = caller_path.read_text()
    move_source = move_path.read_text()
    helper_source = helper_path.read_text()
    patched_source = patch(caller_source, move_source)
    caller = function_body(caller_source, FUNCTION)
    verify_caller(caller_source)

    rotate_start = helper_source.index(ROTATE_BEGIN)
    rotate_end = helper_source.index(ROTATE_END, rotate_start)
    rotate = helper_source[rotate_start:rotate_end]
    if hashlib.sha256(rotate.encode()).hexdigest() != ROTATE_SHA256:
        raise ValueError("generated rotate helpers changed; re-audit extracted oracle")

    move = function_body(move_source, MEMMOVE_FUNCTION)
    if hashlib.sha256(move.encode()).hexdigest() != MEMMOVE_SHA256:
        raise ValueError("generated memmove changed; re-audit extracted oracle")
    move = _rename(move, MEMMOVE_FUNCTION, "reference_memmove")
    move = move.replace("func_00C07818", "reference_memmove_backward")

    cross = function_body(helper_source, "void func_00C07818(ppu_context* ctx) {")
    cross = _rename(cross, "void func_00C07818(ppu_context* ctx) {", "reference_memmove_backward")

    bitfill = function_body(helper_source, "void func_00C0A6B0(ppu_context* ctx) {")
    bitfill = _rename(bitfill, "void func_00C0A6B0(ppu_context* ctx) {", "reference_caller_next_helper")
    next_tail = function_body(helper_source, "void func_00C0A968(ppu_context* ctx) {")
    next_tail = next_tail.replace("void func_00C0A968", "void reference_caller_next_tail", 1)
    bitfill = bitfill.replace("func_00C0A968", "reference_caller_next_tail")

    call_pos = caller.index(CALLSITE) + len(CALLSITE)
    cont_end = caller.index(CONTINUATION_END, call_pos) + len(CONTINUATION_END)
    continuation = caller[call_pos:cont_end]
    continuation = "void audited_caller_continuation(ppu_context* ctx) {" + continuation + "\n}"
    if patched_source.count("haze_slot_copy_try(ctx)") != 1:
        raise ValueError("slot-copy branch was not installed exactly once")

    return rotate, move, cross, bitfill, next_tail, continuation


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", type=Path, help="directory containing generated PPU TUs")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(extract(args.generated)) + "\n")
    print("original memmove SHA256:", MEMMOVE_SHA256)
    print("caller continuation pinned to func_00682648 source")
