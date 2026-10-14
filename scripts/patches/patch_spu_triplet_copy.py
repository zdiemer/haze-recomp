"""Insert an opt-in native prefix into Haze's audited three-word ring copy.

The final translated iteration restores scratch registers; its exit is intact.
Never modify or publish the generated input. A changed image fails closed.
"""
import hashlib

ENTRY = 'void spu_0041_at_00F2E380_spu_func_000035F8(spu_context* ctx) {'
IMAGE_DIGEST = '0cb1ea74176c5242093cb64dc2d3b830b079926ee7361ec2e27ae4d46d8b3ae9'
DIGEST = '17893a3165a2dd8e2d92434ed7cc6a694c03d98b09e0661e47634141c322ac69'
INSERT = '''
        if (haze_spu_triplet_copy_enabled()) {
            unsigned native_items = haze_spu_triplet_copy(ctx);
            if (native_items) haze_spu_triplet_copy_note(native_items);
        }'''


def body(source):
    begin = source.index(ENTRY)
    return source[begin:source.index('\nvoid ', begin + len(ENTRY))]


def patch(source):
    if (hashlib.sha256(source.encode()).hexdigest() != IMAGE_DIGEST or
        hashlib.sha256(body(source).encode()).hexdigest() != DIGEST):
        raise ValueError('Haze triplet-copy image changed; audit before applying the native patch')
    return source.replace('#include "spu_helpers.h"',
        '#include "spu_helpers.h"\n#include "haze_spu_triplet_copy.h"', 1).replace(
        ENTRY, ENTRY + INSERT, 1)
