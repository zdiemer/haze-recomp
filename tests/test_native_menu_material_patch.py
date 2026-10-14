import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patch_menu_material import patch, ENTRY
from patch_menu_pages import patch as pages
from patch_menu_input import patch as input_patch

source = Path(sys.argv[1]).read_bytes().replace(b'\r\n', b'\n').decode('latin1')
changed = patch(source)
assert changed.count('haze_native_popup_material(ctx,widget);') == 1
assert 'haze_original_002F1538(ctx);\n    DRAIN_TRAMPOLINE(ctx);' in changed
assert patch(pages(input_patch(source))) == pages(input_patch(changed))
for bad in (changed, source + '\n' + ENTRY, source.replace(ENTRY, ENTRY + '\n    ctx->gpr[3]=0;')):
    try:
        patch(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Modified container drawing accepted')
print('Native material boundary signature, trampoline drain and patch composition passed.')
