"""Notify the PC page host after Haze activates a retail menu page."""
import argparse
import hashlib
from pathlib import Path

ENTRY = 'void func_00319C68(ppu_context* ctx) {'
DIGEST = 'd75b7a718b454d261a771f67bf9237dbdadb72f0b361e950a8f574d6f83c1d57'
NOTIFY = 'DRAIN_TRAMPOLINE(ctx); haze_menu_page_loaded(ctx,haze_page); return;'

def patch(source):
    if source.count(ENTRY) != 1:
        raise ValueError('Expected exactly one menu page activation function')
    start = source.index(ENTRY)
    end = source.index('\nvoid ', start + len(ENTRY))
    original = source[start:end]
    if hashlib.sha256(original.encode()).hexdigest() != DIGEST:
        raise ValueError('Menu page activation changed; audit the guest hook')
    body = original.replace(ENTRY, ENTRY + '\n        const uint32_t haze_page = (uint32_t)ctx->gpr[4];\n        extern void haze_menu_page_loaded(ppu_context*,uint32_t);', 1)
    # A lifted tail call queues the rest of page activation for its caller.
    # Finish that guest callback before exposing widgets/profile values to the
    # host. The callback can chain further fragments, so drain rather than
    # invoking only its first function. Preserve the captured original page ID.
    body = body.replace('return;', NOTIFY)
    return source[:start] + body + source[end:]

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.source.read_bytes().replace(b'\r\n', b'\n').decode('latin1')
    result = patch(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result.encode('latin1'))
