#include <cassert>
#include <unordered_map>
#include "../port/haze_capture.h"
#include "../port/haze_menu_pointer.cpp"
namespace {
std::unordered_map<uint64_t,uint32_t> memory;
HazeMenuPointer pointer;
bool menu;
constexpr uint32_t manager=0x495e0020,context=0x495b82c0,input=0x495cc250;
constexpr uint32_t page=0x49700000,tutorial=0x49701000;
constexpr uint32_t page_handle=0x00100001,tutorial_handle=0x00200001;
void menu_fixture(uint32_t owner,uint32_t root) {
    memory[owner+0x1f0]=owner+0x1000;
    memory[owner+0x1f4]=3; memory[owner+0x1f8]=8;
    memory[owner+0x1008]=root; memory[owner+0x100c]=page_handle;
    memory[owner+0x1010]=tutorial; memory[owner+0x1014]=tutorial_handle;
    memory[owner+0x28c]=1; memory[owner+0x290]=owner+0x2000;
    memory[owner+0x2000]=page_handle;
}
float test_float(uint32_t address) {
    auto bits=memory[address]; float result; std::memcpy(&result,&bits,4); return result;
}
}
namespace {
bool eligible=true;
unsigned original_calls=0,selection_calls=0;
constexpr uint32_t tabs=0x4961cc20,tab=0x49745780;
void (*tab_hook)(ppu_context*)=nullptr;
}
extern "C" void ppu_register_function(uint64_t a,void (*fn)(ppu_context*)) {
    assert(a==0x95ea8); tab_hook=fn;
}
void func_00095EA8(ppu_context*) { ++original_calls; }
void func_002EFC20(ppu_context* c) { c->gpr[3]=eligible; }
void func_002F2978(ppu_context* c) { write_float(c->gpr[4]+4,.2f); }
void func_00311060(ppu_context* c) {
    auto& remaining=memory[c->gpr[3]+8]; c->gpr[3]=remaining ? tab : 0; remaining=0;
}
void func_00098038(ppu_context* c) {
    assert(c->gpr[4]==tabs && c->gpr[5]==tab && c->fpr[1]==-1.);
    write_float(c->gpr[3],.3f); write_float(c->gpr[3]+8,.4f);
}
void func_00305510(ppu_context* c) {
    assert(c->gpr[3]==tabs && c->gpr[4]==42); ++selection_calls;
}
uint32_t vm_read32(uint64_t a) { return memory[a]; }
uint8_t vm_read8(uint64_t a) { return static_cast<uint8_t>(memory[a]); }
void vm_write32(uint64_t a,uint32_t v) { memory[a]=v; }
void vm_write8(uint64_t a,uint8_t v) { memory[a]=v; }
void haze_input_menu_state(bool active) { menu=active; }
HazeMenuPointer haze_input_take_pointer() { return pointer; }
int main() {
    HazeCapturePolicy p;
    assert(!p.wants_capture());
    p.ready=true; assert(!p.wants_capture()); // Background launch defers capture.
    p.focused=true; assert(p.wants_capture());
    p.toggle(true); assert(!p.wants_capture());
    for (int i=0;i<50;i++) assert(!p.wants_capture()); // Manual release persists.
    p.menu_state(true); p.toggle(false); assert(!p.wants_capture());
    p.menu_state(false); assert(!p.wants_capture());
    p.toggle(false); assert(p.wants_capture());
    p.menu_state(true); assert(!p.wants_capture());
    p.menu_state(false); assert(p.wants_capture());
    p.focused=false; assert(!p.wants_capture());
    p.focused=true; assert(p.wants_capture());
    p.use_controller(); assert(!p.wants_capture());
    p.use_keyboard(); assert(p.wants_capture());
    p.pause_pending=true; assert(!p.wants_capture());
    p.menu_state(true); p.menu_state(false); assert(p.wants_capture());
    p.enabled=false; assert(!p.wants_capture());

    haze_menu_initialize(); assert(!haze_menu_gameplay_allowed());
    memory[0x30d398]=0x8063028c; memory[0x30cee0]=0x80630048;
    memory[0x2e0dd8]=0xf821ff61; memory[0xe16e94]=0xf48dd4;
    memory[0x30d070]=0x38830044; memory[0x31a2c4]=0x981d019c;
    memory[0x4d2da4]=0x800900c0; memory[0x4d2dc0]=0x88030168;
    memory[0x4d2dcc]=0x88030169; memory[0xe176b8]=0x100eac8;
    memory[0xdd5408]=0xe3a630; memory[0xe3a630]=0x95ea8;
    memory[0x95ea8]=0xf821feb1; memory[0xe0b2ec]=0xbf800000;
    haze_menu_initialize();
    assert(tab_hook);
    memory[0xf48dd4]=manager; memory[manager+0x48]=context;
    assert(haze_menu_gameplay_allowed());
    menu_fixture(context,page);
    memory[context+0x170]=input;
    assert(!haze_menu_gameplay_allowed()); // Live state, before publication.
    haze_menu_context(context); assert(menu);
    memory[manager+0x48]=0;
    assert(!haze_menu_gameplay_allowed()); // Current-context slot is temporary.
    memory[manager+0x48]=context;
    haze_menu_pointer_before(input);
    assert(memory[input+0x5c]==0 && memory[input+0x5d]==1);
    pointer={.25f,.75f,true,true,true,false,false,0};
    haze_menu_pointer_after(input);
    assert(memory[input+0x5c]==1 && memory[input+0x5d]==0);
    assert(test_float(input+0x14)==.25f && test_float(input+0x18)==.75f);
    assert(test_float(input+0x1c)==.25f && memory[input+0x4c]==1);
    pointer.x=.5f; pointer.pressed=false;
    haze_menu_pointer_after(input);
    assert(test_float(input+0x1c)==.25f && test_float(input+0x24)==.5f); // Drag origin retained.
    pointer.held=false; pointer.released=true;
    haze_menu_pointer_after(input); assert(memory[input+0x4c]==2);
    assert(haze_menu_pointer_confirm(input,0x49600000,0x49600000));
    assert(!haze_menu_pointer_confirm(input,0x49600000,0x49600010));
    assert(!haze_menu_pointer_confirm(input+4,0x49600000,0x49600000));
    assert(memory[input+0x60]==0);
    ppu_context cpu{}; cpu.gpr[1]=0xd0001000; cpu.gpr[3]=tabs;
    memory[tabs+0x60]=1; memory[tabs+0xcc]=context;
    memory[tab+0x30]=1; memory[tab+0x14c]=0x49620000; memory[tab+0x28]=42;
    write_float(tabs+0x154,.1f);
    write_float(input+0x14,.35f); write_float(input+0x18,.25f);
    tab_hook(&cpu); assert(selection_calls==1 && cpu.gpr[3]==1 && cpu.gpr[1]==0xd0001000);
    eligible=false; cpu.gpr[3]=tabs; tab_hook(&cpu);
    assert(selection_calls==1 && cpu.gpr[3]==0); // Modal/topmost eligibility wins.
    eligible=true; write_float(input+0x14,.29f); cpu.gpr[3]=tabs; tab_hook(&cpu);
    assert(selection_calls==1 && cpu.gpr[3]==0); // Outside the rendered tab.
    write_float(input+0x14,.35f); memory[tab+0x30]=0; cpu.gpr[3]=tabs; tab_hook(&cpu);
    assert(selection_calls==1); // Hidden tabs cannot activate.
    pointer.released=false; pointer.back=true;
    haze_menu_pointer_after(input); assert(memory[input+0x4c]==0 && memory[input+0x50]==2);
    assert(!haze_menu_pointer_confirm(input,0x49600000,0x49600000));
    pointer.back=false; pointer.scroll=-1;
    haze_menu_pointer_after(input);
    assert(memory[input+0x5c]==0 && memory[input+0x5d]==1);
    assert(test_float(input+0x3c)==-1.f);
    pointer.active=false;
    haze_menu_pointer_before(input); haze_menu_pointer_after(input);
    cpu.gpr[3]=tabs; tab_hook(&cpu); assert(original_calls==1);
    assert(memory[input+0x5c]==0 && memory[input+0x5d]==1);
    memory[context+0x28c]=0; haze_menu_context(context);
    assert(!menu && haze_menu_gameplay_allowed());

    // The retail tutorial host is also used for nonblocking gameplay tips.
    menu_fixture(context,page);
    memory[context+0x44]=page_handle;
    memory[page+0x60]=1; memory[page+0x64]=page+0x2000;
    memory[page+0x2000]=tutorial_handle;
    memory[tutorial]=0xde6e88;
    haze_menu_context(context);
    assert(!menu && haze_menu_gameplay_allowed());
    pointer.active=true; pointer.released=true;
    haze_menu_pointer_before(input); haze_menu_pointer_after(input);
    assert(memory[input+0x5c]==0); // A tip cannot acquire the menu pointer.
    memory[tutorial+0x19c]=1;
    assert(!haze_menu_gameplay_allowed()); // Blocking tutorial, before dispatch.
    haze_menu_context(context); assert(menu);
    memory[tutorial+0x19c]=0;
    haze_menu_context(context); assert(!menu && haze_menu_gameplay_allowed());

    // Local-player ownership survives an empty/unrelated last GUI dispatch.
    constexpr uint32_t players=0x48000000,player=0x48100000,other=0x48200000;
    memory[0xf48c3c]=players; memory[players+0x2a0]=1;
    memory[players+0xc0]=player; memory[player+0x9c]=2;
    memory[player+0x168]=memory[player+0x169]=1;
    memory[player+0x1c]=context;
    memory[tutorial+0x19c]=1;
    memory[manager+0x48]=0;
    haze_menu_context(other);
    assert(menu && !haze_menu_gameplay_allowed());
    memory[tutorial+0x19c]=0;
    haze_menu_context(other); assert(!menu && haze_menu_gameplay_allowed());
    menu_fixture(other,0x49900000);
    haze_menu_context(other); assert(!menu && haze_menu_gameplay_allowed());
    memory[other+0x28c]=0; // An unrelated context cannot steal local ownership.

    // A tip must not hide a real pause/settings page in the same stack.
    memory[context+0x28c]=2;
    memory[context+0x1f4]=4;
    memory[context+0x1018]=0x49800000; memory[context+0x101c]=0x00300001;
    memory[context+0x2008]=0x00300001;
    haze_menu_context(other); assert(menu && !haze_menu_gameplay_allowed());
    memory[context+0x28c]=1;
    haze_menu_context(other); assert(!menu && haze_menu_gameplay_allowed());

    // Independent guest pause reasons also block direct camera writes.
    memory[0x100eac8]=0x100; assert(!haze_menu_gameplay_allowed());
    memory[0x100eac8]=0;
    // Reused/stale widget handles cannot be mistaken for a passive tip.
    memory[context+0x1014]=tutorial_handle+1;
    haze_menu_context(other); assert(menu && !haze_menu_gameplay_allowed());
    memory[context+0x1014]=tutorial_handle;
    memory[tutorial]=0xde5ca8; // Another dialog on this host is not a tip.
    assert(!haze_menu_gameplay_allowed());
    memory[tutorial]=0xde6e88;
    memory[context+0x44]=0; haze_menu_context(context); assert(menu);
    memory[player+0x169]=0; // Player no longer owns local input.
    haze_menu_context_reset(context);
    assert(haze_menu_gameplay_allowed());
    memory[context+0x28c]=0; haze_menu_context(other); assert(!menu);
    puts("Menu ownership, native pointer packet, drag/release and capture lifecycle passed.");
}
