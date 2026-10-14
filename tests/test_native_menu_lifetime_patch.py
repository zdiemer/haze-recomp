import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patch_menu_lifetime import patch, ENTRY, NOTIFY
from patch_menu_input import patch as input_patch
from patch_menu_pages import patch as page_patch
from patch_menu_material import patch as material_patch

source = Path(sys.argv[1]).read_bytes().replace(b'\r\n', b'\n').decode('latin1')
modified = patch(source)
assert modified.replace(NOTIFY, '') == source
assert modified.index(NOTIFY) < modified.index('func_00311060(ctx)', modified.index(ENTRY))
assert patch(material_patch(page_patch(input_patch(source)))) == material_patch(page_patch(input_patch(modified)))
for changed in (modified, source.replace(ENTRY, ENTRY + '\n        ctx->gpr[3]=0;', 1), source + '\n' + ENTRY):
    try:
        patch(changed)
    except ValueError:
        pass
    else:
        raise AssertionError('Changed/duplicate destruction boundary accepted')
print('Native widget lifetime hook runs before destruction; signatures and patch composition passed.')
