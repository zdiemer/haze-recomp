"""Extract the actual audited local loop for native-copy differential testing."""
import argparse
from pathlib import Path
from .patch_spu_triplet_copy import body, patch, ENTRY

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    source = a.source.read_text()
    baseline = body(source).replace(ENTRY.split('(')[0].split()[-1], 'haze_triplet_reference')
    candidate = body(patch(source)).replace(ENTRY.split('(')[0].split()[-1], 'haze_triplet_candidate')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text('#include "spu_helpers.h"\n#include "haze_spu_triplet_copy.h"\n'
        'static void spu_0041_at_00F2E380_spu_func_00003690(spu_context* ctx) { (void)ctx; }\n'
        + baseline + '\n' + candidate + '\n')
