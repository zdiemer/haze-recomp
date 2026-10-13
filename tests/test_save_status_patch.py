import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from patch_save_status import patch, DIGESTS
from patch_guest_pacer import patch as pacing
source=Path(sys.argv[1]).read_bytes().replace(b'\r\n',b'\n').decode('latin1')
modified=patch(source)
restored=modified
for inserted in [
    '\n        extern uint64_t haze_save_begin(uint32_t);\n        extern void haze_save_launched(uint64_t,int32_t);\n        const uint64_t haze_save_ticket=haze_save_begin((uint32_t)ctx->gpr[3]);',
    '\n        extern uint64_t haze_save_current();\n        extern void haze_save_finished(uint64_t,int32_t);\n        const uint64_t haze_save_ticket=haze_save_current();\n        int32_t haze_save_result=INT32_MIN;',
    '\n        haze_save_result=(int32_t)ctx->gpr[29];',
    '        haze_save_finished(haze_save_ticket,haze_save_result);\n',
    'haze_save_launched(haze_save_ticket,(int32_t)ctx->gpr[3]); ',
]:
    assert restored.count(inserted)==1
    restored=restored.replace(inserted,'')
assert restored==source
assert patch(pacing(source))==pacing(patch(source))
for address in DIGESTS:
    entry=f'void func_{address}(ppu_context* ctx) {{'
    for changed in [source+'\n'+entry,source.replace(entry,entry+'\n        ctx->gpr[3]=0;'),modified]:
        try: patch(changed)
        except ValueError: pass
        else: raise AssertionError('Unaudited save function accepted')
print('Save observation preserves original worker/launcher and composes with pacing; changed lifts rejected.')
