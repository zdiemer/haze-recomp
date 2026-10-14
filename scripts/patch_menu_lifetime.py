"""Invalidate host widget identities before Haze destroys a native GUI context."""
import argparse
import hashlib
from pathlib import Path

ENTRY = 'void func_00317300(ppu_context* ctx) {'
DIGEST = '5114afc6f9bcaaf1b171016781d05e227f3764e7917417108357c177debacd0b'
NOTIFY = '\n        extern void haze_native_menu_context_reset(uint32_t);\n        haze_native_menu_context_reset(static_cast<uint32_t>(ctx->gpr[3]));'


def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected one GUI root destruction function')
    start = source.index(ENTRY)
    end = source.index('\nvoid ', start + len(ENTRY))
    original = source[start:end]
    if hashlib.sha256(original.encode()).hexdigest() != DIGEST:
        raise ValueError('GUI destruction changed; audit native widget ownership')
    return source[:start] + original.replace(ENTRY, ENTRY + NOTIFY, 1) + source[end:]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.source.read_bytes().replace(b'\r\n', b'\n').decode('latin1')
    result = patch(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result.encode('latin1'))
