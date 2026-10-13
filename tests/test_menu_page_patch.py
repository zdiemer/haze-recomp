import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from patch_menu_pages import patch, ENTRY
source=Path(sys.argv[1]).read_bytes().replace(b'\r\n',b'\n').decode('latin1')
modified=patch(source)
assert modified.count('haze_menu_page_loaded(haze_page);')==2
restored=modified.replace('\n        const uint32_t haze_page = (uint32_t)ctx->gpr[4];\n        extern void haze_menu_page_loaded(uint32_t);','').replace('haze_menu_page_loaded(haze_page); return;','return;')
assert restored==source
for changed in (modified,source.replace(ENTRY,ENTRY+'\n        ctx->gpr[4]=0;',1),source+'\n'+ENTRY):
    try: patch(changed)
    except ValueError: pass
    else: raise AssertionError('Changed page activation was accepted')
print('Menu page patch preserves the original lift and rejects changed/duplicate boundaries.')
