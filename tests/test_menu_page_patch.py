import sys
import re
import subprocess
import tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from patches.patch_menu_pages import patch, ENTRY, NOTIFY
from patches.patch_menu_input import patch as pointer_patch
source=Path(sys.argv[1]).read_bytes().replace(b'\r\n',b'\n').decode('latin1')
modified=patch(source)
assert modified.count('haze_menu_page_loaded(ctx,haze_page);')==2
restored=modified.replace('\n        const uint32_t haze_page = (uint32_t)ctx->gpr[4];\n        extern void haze_menu_page_loaded(ppu_context*,uint32_t);','').replace(NOTIFY,'return;')
assert restored==source
# Both features patch the same generated translation unit. Neither may replace
# the other's output when configured together, and their guards remain intact.
combined=patch(pointer_patch(source))
assert combined==pointer_patch(modified)
assert combined.count('haze_menu_page_loaded(ctx,haze_page);')==2
assert combined.count('haze_menu_pointer_after(input);')==1
assert combined.count('haze_menu_context(static_cast<uint32_t>(ctx->gpr[3]));')==1
assert combined.count('haze_original_0031B9C0(ctx);\n    DRAIN_TRAMPOLINE(ctx);')==1
assert combined.count('haze_audio_guest_tick(ctx);')==1
for changed in (modified,source.replace(ENTRY,ENTRY+'\n        ctx->gpr[4]=0;',1),source+'\n'+ENTRY):
    try: patch(changed)
    except ValueError: pass
    else: raise AssertionError('Changed page activation was accepted')

# Execute the actual generated tail-return epilogue with the lift's own
# trampoline macro. Page initialization may itself queue another fragment;
# the host callback must observe both fragments completed and the original ID.
start=modified.index(ENTRY)
end=modified.index('\nvoid ',start+len(ENTRY))
body=modified[start:end]
tail=re.search(r'\{ g_trampoline_fn = \(void\(\*\)\(void\*\)\)func_00313E70;[^\n]+\}',body)
assert tail, 'Audited page callback tail path disappeared'
macro_start=source.index('#define DRAIN_TRAMPOLINE(ctx)')
macro_end=source.index('} while(0)',macro_start)+len('} while(0)')
harness='''#include <cstdint>
struct ppu_context { uint64_t gpr[32]{}; };
static void (*g_trampoline_fn)(void*);
static int stage,callbacks,failure;
'''+source[macro_start:macro_end]+'''
void finish_initialization(void* raw) {
    auto* ctx=static_cast<ppu_context*>(raw);
    stage=2; ctx->gpr[4]=99;
}
void func_00313E70(ppu_context*) {
    stage=1; g_trampoline_fn=finish_initialization;
}
void haze_menu_page_loaded(ppu_context*,uint32_t page) {
    ++callbacks;
    if (stage!=2 || g_trampoline_fn || page!=42) failure=1;
}
void patched_tail(ppu_context* ctx) {
    const uint32_t haze_page=static_cast<uint32_t>(ctx->gpr[4]);
'''+tail.group(0)+'''
}
int main() {
    ppu_context ctx; ctx.gpr[4]=42;
    patched_tail(&ctx);
    DRAIN_TRAMPOLINE(&ctx); // The lifted caller also drains after returning.
    return failure || callbacks!=1 || stage!=2 || ctx.gpr[4]!=99;
}
'''
assert len(sys.argv)>2, 'Pass the configured C++ compiler as the second argument'
with tempfile.TemporaryDirectory(prefix='haze-page-tail-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run([sys.argv[2],'/nologo','/std:c++20','test.cpp','/Fe:test.exe'],cwd=path,check=True)
    completed=subprocess.run([str(path/'test.exe')],cwd=path)
    assert completed.returncode==0, 'Native page callback ran before guest initialization completed'
print('Menu page patch preserves the original lift and rejects changed/duplicate boundaries.')
print('Menu page tail callback completes chained guest initialization before notifying the host.')
