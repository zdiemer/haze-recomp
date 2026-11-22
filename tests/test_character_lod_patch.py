"""Audit the character LOD selection patch and execute both setting states."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patches.patch_character_lod import patch, ENTRY, SITES, DECLARATION

source = Path(sys.argv[1]).read_bytes().replace(b'\r\n', b'\n').decode('latin1')
modified = patch(source)
start = modified.index(ENTRY)
body = modified[start:modified.index('\nvoid ', start + len(ENTRY))]
# Only the two reduced-mask stores and one declaration change; the +3C, +6C
# and every other path keep the original translation.
restored = modified.replace(ENTRY + DECLARATION, ENTRY, 1)
for original, replacement in SITES.items():
    assert body.count(replacement) == 1
    restored = restored.replace(replacement, original, 1)
assert restored == source
assert 'ctx->gpr[0] = ctx->gpr[28] + (int64_t)(0x3C);' in body
assert 'ctx->gpr[0] = ctx->gpr[28] + (int64_t)(0x6C);' in body
first = next(iter(SITES))
for rejected in (source.replace(first, first + '\n', 1), source + '\n' + ENTRY + '\n}\n'):
    try:
        patch(rejected)
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted an unaudited character LOD selection')

port = Path(__file__).resolve().parents[1] / 'port'
cases = '\n'.join(f'    {{ ppu_context state, *ctx=&state; ctx->gpr[28]=0x1000; {replacement} results.push_back(uint32_t(ctx->gpr[0]-0x1000)); }}'
                  for replacement in SITES.values())
harness = '''#include <cstdint>
#include <cassert>
#include <vector>
#include "haze_character_lod.h"
#include "haze_character_lod.cpp"
struct ppu_context { uint64_t gpr[32]{}; };
std::vector<uint32_t> run() {
    std::vector<uint32_t> results;
''' + cases + '''
    return results;
}
int main() {
    assert(haze_live_character_full_detail);
    assert((run()==std::vector<uint32_t>{0x3C,0x3C}));
    haze_live_character_full_detail=false;
    assert((run()==std::vector<uint32_t>{0x4C,0x5C}));
    assert(haze_character_detail_full("full") && !haze_character_detail_full("original"));
}
'''
with tempfile.TemporaryDirectory(prefix='haze-character-lod-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run([sys.argv[2], '/nologo', '/std:c++20', f'/I{port}', 'test.cpp', '/Fe:test.exe'],
                   cwd=path, check=True)
    subprocess.run([str(path / 'test.exe')], cwd=path, check=True)
print('Character LOD selection guard, unchanged +3C/+6C paths and full/original masks pass.')
