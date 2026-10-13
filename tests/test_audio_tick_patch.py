import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from patch_audio_tick import patch, DECL, CALL, ENTRY
from patch_guest_pacer import patch as pacing
from patch_save_status import patch as saves
source=Path(sys.argv[1]).read_bytes().replace(b'\r\n',b'\n').decode('latin1')
base=saves(pacing(source)); modified=patch(base)
assert modified.count(DECL)==1 and modified.count(CALL)==1
assert 'loc_006812DC:\n'+CALL in modified
assert 'loc_00681358:\n'+CALL not in modified
assert modified.replace(DECL,'').replace(CALL,'')==base
for changed in [source,modified,base+'\n'+ENTRY,base.replace(ENTRY,ENTRY+'\nctx->gpr[3]=0;')]:
    try: patch(changed)
    except ValueError: pass
    else: raise AssertionError('Unaudited audio tick source accepted')
print('Audio frame hook preserves pacing/save hooks and rejects changed frame code.')
