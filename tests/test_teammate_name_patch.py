"""Audit the guest hook and execute its bounded display-only copy."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patches.patch_teammate_name import patch, ENTRY, SITE, COPY

source = Path(sys.argv[1]).read_text()
modified = patch(source)
assert modified.replace(SITE + COPY, SITE, 1) == source
assert modified.index(COPY) > modified.index('ctx->lr = 0x0062A890; func_004D11F0(ctx);')
for changed in (modified, source + '\n' + ENTRY, source.replace(SITE, SITE + '\n', 1)):
    try:
        patch(changed)
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted an unaudited teammate update')

harness = '''#include <cstdint>
#include <cstring>
#include <cassert>
#include <algorithm>
uint8_t memory[8192];
uint8_t vm_read8(uint64_t a) { assert(a<sizeof memory); return memory[a]; }
void vm_write8(uint64_t a,uint8_t b) { assert(a<sizeof memory); memory[a]=b; }
uint32_t vm_read32(uint64_t a) { return uint32_t(vm_read8(a))<<24 | uint32_t(vm_read8(a+1))<<16 | uint32_t(vm_read8(a+2))<<8 | vm_read8(a+3); }
void word(uint32_t a,uint32_t b) { for (unsigned i=0;i<4;++i) memory[a+i]=uint8_t(b>>(24-i*8)); }
struct ppu_context { uint64_t gpr[32]{}; };
void copy(ppu_context* ctx) {
''' + COPY + '''
}
int main() {
    ppu_context ctx; ctx.gpr[31]=0x100; ctx.gpr[3]=0x400; ctx.gpr[0]=0x4c4;
    ctx.gpr[28]=0xc00; ctx.gpr[29]=0x800;
    // The voice lookup returns the local player. Match the viewed actor to
    // another slot, using the full entity handle including its generation.
    word(0x8c0,0x400); word(0x8dc,0x600);
    word(0x560,0x1380032d); word(0x760,0x1370032c); word(0xc44,0x1370032c);
    std::memcpy(memory+0x56a,"Local_46",9);
    memory[0x256]=0x10; // owner+154 -> label 1000
    std::fill(memory+0x1000,memory+0x1100,0xA5);
    std::memcpy(memory+0x1000+0x35,"User",5);
    std::memcpy(memory+0x76a,"Remote_46",10);
    auto before=ctx;
    copy(&ctx);
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x1035),"Remote_46"));
    assert(!std::memcmp(&before,&ctx,sizeof ctx));
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x76a),"Remote_46"));
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x56a),"Local_46"));
    assert(memory[0x1034]==0xA5 && memory[0x1075]==0xA5);
    // A malformed unterminated remote field cannot overwrite label state.
    std::fill(memory+0x76a,memory+0x7aa,'X');
    copy(&ctx);
    assert(memory[0x1073]=='X' && memory[0x1074]==0 && memory[0x1075]==0xA5);
    // Missing display data retains the original character-name fallback.
    std::memcpy(memory+0x1035,"Fallback",9);memory[0x76a]=0;copy(&ctx);
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x1035),"Fallback"));
    memory[0x256]=0;memory[0x76a]='Y';copy(&ctx);
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x1035),"Fallback"));
    memory[0x256]=0x10;word(0xc44,0x1370032d);copy(&ctx); // stale handle
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x1035),"Fallback"));
    word(0xc44,0);copy(&ctx); // NPC/unassigned entity
    assert(!std::strcmp(reinterpret_cast<char*>(memory+0x1035),"Fallback"));
}
'''
with tempfile.TemporaryDirectory(prefix='haze-teammate-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run([sys.argv[2], '/nologo', '/std:c++20', 'test.cpp', '/Fe:test.exe'], cwd=path, check=True)
    subprocess.run([str(path / 'test.exe')], cwd=path, check=True)
print('Teammate label guard, remote-name copy, bounds, fallback and register preservation pass.')
