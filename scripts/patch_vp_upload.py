"""Install the guarded full-block VP packet replacement in a build-local copy."""
import argparse
import hashlib
from pathlib import Path
from extract_vp_packet_oracle import FUNCTION_SHA256

DECL = '        extern bool haze_vp_upload_begin(ppu_context*);\n        extern void haze_vp_upload_end(ppu_context*);\n'
BEGIN = '        if (haze_vp_upload_begin(ctx)) goto haze_vp_upload_done;\n'
END = 'haze_vp_upload_done:\n        haze_vp_upload_end(ctx);\n'
TAIL = '        ctx->gpr[0] = (uint64_t)ppc_rlwinm((uint32_t)ctx->gpr[31], 7, 0, 24);'

def patch(source):
    a=source.index('void func_0072C368(ppu_context* ctx) {')
    b=source.index('\nvoid func_',a+1)
    body=source[a:b]
    if hashlib.sha256(body.encode()).hexdigest()!=FUNCTION_SHA256:
        raise ValueError('Haze VP upload function changed; re-audit the boundary')
    assert body.count('loc_0072C4F8:')==1 and body.count(TAIL)==1
    changed=body.replace('{\n','{\n'+DECL,1)
    changed=changed.replace('loc_0072C4F8:',BEGIN+'loc_0072C4F8:',1)
    changed=changed.replace(TAIL,END+TAIL,1)
    return source[:a]+changed+source[b:]

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();result=patch(args.source.read_text())
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(result)
