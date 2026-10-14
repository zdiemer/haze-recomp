"""Consume queued audio edits at Haze's guest frame boundary, after pacing patch."""
import argparse
import hashlib
from pathlib import Path
ENTRY='void func_00680FA0(ppu_context* ctx) {'
DIGEST='56f434e9a85787e2471f2569106c05ee280af91f84d1b37efeecf10ffbf0ae1e'
# Same paced function after the audited producer scopes: 59 direct calls and
# two indirect calls are wrapped, with the original instructions preserved.
# The convergence label remains outside those call scopes. Both variants keep
# exact digest guards so an unrelated source change still requires an audit.
PROFILED_DIGEST='e9c026d348865c105f8c2c839d5e0382ac8fb48e8e9c8cb55b7118c88bea44a7'
DECL='\n        extern void haze_audio_guest_tick(ppu_context*);\n        extern void haze_quit_guest_tick(ppu_context*);'
# All frame paths converge here, including menus that bypass the optional
# guest pacing branch at 00681358 entirely.
SITE='loc_006812DC:\n'
CALL='        haze_audio_guest_tick(ctx);\n        haze_quit_guest_tick(ctx);\n'
def patch(source):
    if source.count(ENTRY)!=1: raise ValueError('Expected one guest frame function')
    start=source.index(ENTRY); end=source.index('\nvoid ',start+len(ENTRY))
    original=source[start:end]
    if hashlib.sha256(original.encode()).hexdigest() not in (DIGEST,PROFILED_DIGEST):
        raise ValueError('Paced guest frame changed; audit audio command boundary')
    if original.count(SITE)!=1: raise ValueError('Audio command boundary changed')
    body=original.replace(ENTRY,ENTRY+DECL,1).replace(SITE,SITE+CALL,1)
    return source[:start]+body+source[end:]
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path); parser.add_argument('output',type=Path)
    args=parser.parse_args()
    result=patch(args.source.read_bytes().replace(b'\r\n',b'\n').decode('latin1'))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(result.encode('latin1'))
