"""Exercise the audited patch against the actual local lift, without distributing it."""
import argparse
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import patch_title_prompt as patcher

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
args = parser.parse_args()
original = args.source.read_text()
patched = patcher.patch(original)
assert patched.count('haze_title_prepare_text((uint32_t)') == 1
assert patched.replace(
    '\n        extern void haze_title_prepare_text(uint32_t, uint32_t, uint32_t);'
    '\n        haze_title_prepare_text((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);',
    '', 1) == original
for invalid in (patched, original.replace(patcher.ENTRY, patcher.ENTRY + '\n        /* changed */', 1),
                original.replace(patcher.ENTRY, 'void unknown_title_lookup(ppu_context* ctx) {', 1)):
    try:
        patcher.patch(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Changed or already-patched lookup must fail closed')
print('Title patch preserves the complete lift and rejects changed/duplicate boundaries.')
