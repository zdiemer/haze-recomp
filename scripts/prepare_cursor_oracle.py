"""Extract original caller blocks for a native-kernel boundary comparison."""
import argparse
from pathlib import Path
from patch_native_cursor import PREFIX, body, patch

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    source = a.source.read_text()
    changed = patch(source)
    text = '#include "spu_helpers.h"\n#include "haze_native_cursor_bridge.h"\n'
    for address in ['000048C8', '000047F8', '00004878', '00004918']:
        text += f'static void {PREFIX}{address}(spu_context* ctx) {{ (void)ctx; }}\n'
    text += body(source, '00003450') + '\n'
    for address in ['000047E0', '00004900']:
        text += body(source, address).replace(PREFIX + address, 'haze_cursor_reference_' + address) + '\n'
        text += body(changed, address).replace(PREFIX + address, 'haze_cursor_candidate_' + address) + '\n'
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(text)
