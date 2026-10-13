import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from patch_menu_pages import patch, ENTRY
from patch_menu_input import patch as pointer_patch
source=Path(sys.argv[1]).read_bytes().replace(b'\r\n',b'\n').decode('latin1')
modified=patch(source)
assert modified.count('haze_menu_page_loaded(ctx,haze_page);')==2
restored=modified.replace('\n        const uint32_t haze_page = (uint32_t)ctx->gpr[4];\n        extern void haze_menu_page_loaded(ppu_context*,uint32_t);','').replace('haze_menu_page_loaded(ctx,haze_page); return;','return;')
assert restored==source
# Both features patch the same generated translation unit. Neither may replace
# the other's output when configured together, and their guards remain intact.
combined=patch(pointer_patch(source))
assert combined==pointer_patch(modified)
assert combined.count('haze_menu_page_loaded(ctx,haze_page);')==2
assert combined.count('haze_menu_pointer_after(input);')==1
assert combined.count('haze_menu_context(static_cast<uint32_t>(ctx->gpr[3]));')==1
for changed in (modified,source.replace(ENTRY,ENTRY+'\n        ctx->gpr[4]=0;',1),source+'\n'+ENTRY):
    try: patch(changed)
    except ValueError: pass
    else: raise AssertionError('Changed page activation was accepted')
print('Menu page patch preserves the original lift and rejects changed/duplicate boundaries.')
