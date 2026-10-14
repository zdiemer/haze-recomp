"""Extract baseline and patched slot-fill functions from the private PPU lift."""
from __future__ import annotations

import hashlib
from pathlib import Path

from patch_bulk_fill import (
    CALLER_FUNCTION,
    CALLER_SHA256,
    FUNCTION,
    FUNCTION_SHA256,
    _body,
    patch,
    verify_caller,
)

HELPERS_SHA256 = "ef126c77ecdd331a0577925f7b8bfb695ba9b9480251723a581916744a23675c"
HELPERS_BEGIN = "/* Rotate helpers */"
HELPERS_END = "static inline int64_t ppc_sraw"


def extract_helpers(source: str) -> str:
    start = source.index(HELPERS_BEGIN)
    end = source.index(HELPERS_END, start)
    helpers = source[start:end]
    if hashlib.sha256(helpers.encode()).hexdigest() != HELPERS_SHA256:
        raise ValueError("generated PPU rotate helpers changed; re-audit the test oracle")
    return helpers


def extract(source: str, caller_source: str) -> tuple[str, str]:
    verify_caller(caller_source)
    original = _body(source, FUNCTION)
    if hashlib.sha256(original.encode()).hexdigest() != FUNCTION_SHA256:
        raise ValueError("bulk-fill source changed; re-audit before building an oracle")
    patched_source = patch(source, caller_source)
    patched = _body(patched_source, FUNCTION)
    if original == patched:
        raise ValueError("patcher did not install the native branch")
    return original.replace(FUNCTION, "void reference_fill(ppu_context* ctx) {", 1), \
        patched.replace(FUNCTION, "void patched_fill(ppu_context* ctx) {", 1)


def fixture_functions(generated_dir: Path) -> tuple[str, str, str]:
    source = (generated_dir / "ppu_recomp_006.cpp").read_text()
    caller_source = (generated_dir / "ppu_recomp_003.cpp").read_text()
    return extract_helpers(source), *extract(source, caller_source)


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", type=Path, help="directory containing generated PPU TUs")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    helpers, original, patched = fixture_functions(args.generated)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(helpers + "\n" + original + "\n" + patched + "\n")
    print("bulk-fill function SHA256:", FUNCTION_SHA256)
    print("slot caller SHA256:", CALLER_SHA256)
