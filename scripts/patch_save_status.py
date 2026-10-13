"""Observe native save-worker launch and completion without replacing its work."""
import argparse
import hashlib
from pathlib import Path

DIGESTS = {
    '00721F48': '1e693edfb13e25df9abb67530474b3f98bbfab473cfe5d1d4a2b95f723a3fdbc',
    '007222B0': '0abab85dad859ac6e92fe67898d08e9958238178132c96bb1db0adc5b464439d',
}

def patch(source):
    for address,digest in DIGESTS.items():
        entry=f'void func_{address}(ppu_context* ctx) {{'
        if source.count(entry)!=1:
            raise ValueError('Expected one native save function: '+address)
        start=source.index(entry)
        end=source.index('\nvoid ',start+len(entry))
        original=source[start:end]
        if hashlib.sha256(original.encode()).hexdigest()!=digest:
            raise ValueError('Native save function changed; audit '+address)
        if address=='00721F48':
            body=original.replace(entry,entry+'\n        extern uint64_t haze_save_begin(uint32_t);\n        extern void haze_save_launched(uint64_t,int32_t);\n        const uint64_t haze_save_ticket=haze_save_begin((uint32_t)ctx->gpr[3]);',1)
            body=body.replace('return;','haze_save_launched(haze_save_ticket,(int32_t)ctx->gpr[3]); return;')
            # Admission and shutdown sealing share one mutex. Refusing here,
            # before the original sets its busy flag or creates a thread,
            # closes the gap between observing idle and stopping the host.
            admission='        const uint64_t haze_save_ticket=haze_save_begin((uint32_t)ctx->gpr[3]);'
            body=body.replace(admission,admission+'\n        if (haze_save_ticket==UINT64_MAX) { ctx->gpr[3]=uint64_t(int64_t(-1)); return; }',1)
        else:
            body=original.replace(entry,entry+'\n        extern uint64_t haze_save_current();\n        extern void haze_save_finished(uint64_t,int32_t);\n        const uint64_t haze_save_ticket=haze_save_current();\n        int32_t haze_save_result=INT32_MIN;',1)
            result_site='        vm_write32(ctx->gpr[9] + 0x0, ctx->gpr[29]);'
            if body.count(result_site)!=1:
                raise ValueError('Native save worker result changed')
            body=body.replace(result_site,result_site+'\n        haze_save_result=(int32_t)ctx->gpr[29];',1)
            exit_call='        ctx->lr = 0x007223E8; func_00C27C4C(ctx); DRAIN_TRAMPOLINE(ctx);'
            if body.count(exit_call)!=1:
                raise ValueError('Native save worker exit changed')
            body=body.replace(exit_call,'        haze_save_finished(haze_save_ticket,haze_save_result);\n'+exit_call,1)
        source=source[:start]+body+source[end:]
    return source

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    source=args.source.read_bytes().replace(b'\r\n',b'\n').decode('latin1')
    result=patch(source)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(result.encode('latin1'))
