#include "ppu_recomp.h"
#include "haze_menu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

extern "C" void ppu_register_function(uint64_t,void (*)(ppu_context*));

namespace {
bool supported=false;
bool pointer_owned=false;
bool pointer_released=false;
std::atomic<uint32_t> active_context{0};
bool trace=false;
bool valid(uint32_t address) { return address>=0x10000 && address<0xc0000000 && !(address&3); }
void write_float(uint32_t address,float value) {
    uint32_t bits; std::memcpy(&bits,&value,sizeof(bits)); vm_write32(address,bits);
}
uint32_t current_context() {
    const auto manager=vm_read32(0xf48dd4);
    return valid(manager) ? vm_read32(manager+0x48) : 0;
}
uint32_t resolve_widget(uint32_t context,uint32_t handle) {
    // Read-only form of 0030CF48: handles include a slot generation.
    const auto index=handle>>20;
    const auto data=vm_read32(context+0x1f0),stride=vm_read32(context+0x1f8);
    if (!handle || index>=vm_read32(context+0x1f4) || stride<8 || stride>0x100 || !valid(data)) return 0;
    const uint64_t slot=uint64_t(data)+uint64_t(index)*stride;
    if (slot+8>=0xc0000000 || vm_read32(slot+4)!=handle) return 0;
    const auto widget=vm_read32(slot);
    return valid(widget) ? widget : 0;
}
bool passive_tutorial(uint32_t context,uint32_t page) {
    // 0031A070 attaches tutorialBox to the reusable page at context+44.
    // Its +19c byte distinguishes a blocking tutorial from an in-game tip.
    // Both activate the same page and therefore increase the GUI stack count.
    if (page!=resolve_widget(context,vm_read32(context+0x44))) return false;
    const auto count=vm_read32(page+0x60),data=vm_read32(page+0x64);
    if (!count || count>256 || !valid(data) || uint64_t(data)+count*8>=0xc0000000) return false;
    bool found=false;
    for (uint32_t i=0;i<count;++i) {
        const auto child=resolve_widget(context,vm_read32(data+i*8));
        if (!child) continue;
        if (vm_read32(child)!=0x00de6e88) return false;
        if (vm_read8(child+0x19c)) return false;
        found=true;
    }
    return found;
}
bool has_menu(uint32_t context) {
    if (!valid(context)) return false;
    const auto count=vm_read32(context+0x28c),data=vm_read32(context+0x290);
    if (!count) return false;
    if (count>256 || !valid(data) || uint64_t(data)+count*8>=0xc0000000) return true;
    for (uint32_t i=0;i<count;++i) {
        const auto page=resolve_widget(context,vm_read32(data+i*8));
        if (page && !passive_tutorial(context,page)) return true;
    }
    return false;
}
bool title_page(uint32_t context) {
    // BLUS30094's frontend resource assigns startButton to fefl_DC_page3.
    // +50/+54 is optional navigation history, empty at the live title. Read
    // the top active page handle instead, using the same stack as has_menu().
    // A dialog above the title must still use Confirm, even if history names
    // the title or the title remains underneath it in the active stack.
    if (!valid(context)) return false;
    const auto count=vm_read32(context+0x28c),stack=vm_read32(context+0x290);
    if (!count || count>256 || !valid(stack) || uint64_t(stack)+count*8>=0xc0000000) return false;
    const auto page=resolve_widget(context,vm_read32(stack+(count-1)*8));
    if (!page || uint64_t(page)+0x28>0xc0000000) return false;
    char name[17]{};
    for (unsigned i=0;i<16;++i) name[i]=char(vm_read8(page+0x18+i));
    return !std::strcmp(name,"fefl_DC_page3");
}
bool owns_input(bool* title=nullptr) {
    if (title) *title=false;
    // 004D2D48 enumerates these live local players. A later empty GUI dispatch
    // must not overwrite a pause/tutorial menu owned by another context.
    const auto players=vm_read32(0xf48c3c);
    bool local=false;
    if (valid(players) && vm_read8(players+0x2a0)) {
        for (unsigned i=0;i<32;++i) {
            const auto player=vm_read32(players+0xc0+i*4);
            if (valid(player) && vm_read32(player+0x9c) &&
                vm_read8(player+0x168) && vm_read8(player+0x169)) {
                local=true;
                if (has_menu(vm_read32(player+0x1c))) return true;
            }
        }
    }
    // Frontend/startup has no local player yet. Its current context may be
    // temporary, so retain the last dispatched one until reset/destruction.
    if (local) return false;
    const auto current=current_context();
    const auto context=has_menu(current) ? current : active_context.load();
    if (!has_menu(context)) return false;
    if (title) *title=title_page(context);
    return true;
}
float read_float(uint32_t address) {
    auto bits=vm_read32(address); float value; std::memcpy(&value,&bits,4); return value;
}
void update_tabs(ppu_context* ctx) {
    if (!pointer_owned || !pointer_released || !has_menu(active_context)) {
        func_00095EA8(ctx);
        return;
    }
    // This derived tab widget draws with 00098038, but its original input
    // routine uses base-widget widths and omits the prompt margin/scroll.
    // Reuse the draw layout and the guest's eligibility and selection methods.
    const auto widget=static_cast<uint32_t>(ctx->gpr[3]);
    auto call=*ctx;
    call.gpr[4]=1;
    func_002EFC20(&call);
    if (!call.gpr[3]) { ctx->gpr[3]=0; return; }
    const auto input=vm_read32(active_context+0x170);
    const float x=read_float(input+0x14),y=read_float(input+0x18);
    // A private guest stack frame holds output structs across nested calls.
    const auto stack=static_cast<uint32_t>(ctx->gpr[1])-0x100;
    const auto iterator=stack+0x80,rect=stack+0xa0,bounds=stack+0xb0;
    call=*ctx; call.gpr[1]=stack; call.gpr[3]=widget; call.gpr[4]=rect;
    func_002F2978(&call);
    const float top=read_float(rect+4),height=read_float(widget+0x154);
    vm_write32(iterator,0); vm_write32(iterator+4,widget+0x58);
    vm_write32(iterator+8,vm_read32(widget+0x60));
    vm_write32(iterator+12,vm_read32(widget+0xcc));
    for (;;) {
        call=*ctx; call.gpr[1]=stack; call.gpr[3]=iterator;
        func_00311060(&call);
        const auto child=static_cast<uint32_t>(call.gpr[3]);
        if (!valid(child)) break;
        if (!(vm_read32(child+0x30)&1) || !vm_read32(child+0x14c)) continue;
        call=*ctx; call.gpr[1]=stack; call.gpr[3]=bounds;
        call.gpr[4]=widget; call.gpr[5]=child; call.fpr[1]=-1.;
        func_00098038(&call);
        if (x<=read_float(bounds) || x>=read_float(bounds+8) || y<=top || y>=top+height)
            continue;
        const auto selected=vm_read32(child+0x28);
        if (selected!=vm_read32(widget+0x158)) {
            call=*ctx; call.gpr[1]=stack; call.gpr[3]=widget; call.gpr[4]=selected;
            func_00305510(&call);
        }
        ctx->gpr[3]=1;
        return;
    }
    ctx->gpr[3]=0;
}
}

void haze_menu_initialize() {
    trace=std::getenv("HAZE_MOUSE_TRACE")!=nullptr;
    supported=vm_read32(0x30d398)==0x8063028c &&
              vm_read32(0x30cee0)==0x80630048 &&
              vm_read32(0x2e0dd8)==0xf821ff61 &&
              vm_read32(0xe16e94)==0xf48dd4 &&
              vm_read32(0x30d070)==0x38830044 &&
              vm_read32(0x31a2c4)==0x981d019c &&
              vm_read32(0x4d2da4)==0x800900c0 &&
              vm_read32(0x4d2dc0)==0x88030168 &&
              vm_read32(0x4d2dcc)==0x88030169 &&
              vm_read32(0xe176b8)==0x100eac8;
    std::fprintf(stderr,"[input] Guest menu pointer bridge %s\n",supported ? "enabled" : "unavailable: unsupported layout");
    // 00095EA8 is virtual-only in the audited lift (vtable DD5408).
    if (supported && vm_read32(0xdd5408)==0xe3a630 &&
        vm_read32(0xe3a630)==0x95ea8 && vm_read32(0x95ea8)==0xf821feb1 &&
        vm_read32(0xe0b2ec)==0xbf800000)
        ppu_register_function(0x95ea8,update_tabs);
}
bool haze_menu_gameplay_allowed() {
    // Called on the guest simulation thread, immediately before camera writes.
    // Check live ownership even if the window thread has not processed release.
    // Use persistent local-player contexts between temporary GUI dispatches.
    return supported && !owns_input() && vm_read32(0x100eac8)==0;
}
void haze_menu_context(uint32_t context) {
    if (!supported) return;
    active_context=context;
    bool title=false;
    const bool owned=owns_input(&title);
    haze_input_menu_state(owned,title);
    if (trace) {
        static uint32_t previous_context=0,previous_count=~0u;
        static bool previous_owned=false,previous_title=false;
        const auto count=valid(context) ? vm_read32(context+0x28c) : 0;
        if (previous_context!=context || previous_count!=count || previous_owned!=owned || previous_title!=title)
            std::fprintf(stderr,"[menu] context=%08X stack=%u input=%08X owned=%d title=%d\n",context,count,
                valid(context) ? vm_read32(context+0x170) : 0,owned,title);
        previous_context=context; previous_count=count; previous_owned=owned; previous_title=title;
    }
}
void haze_menu_context_reset(uint32_t context) {
    if (active_context==context) {
        active_context=0;
        pointer_owned=pointer_released=false;
    }
}
void haze_menu_pointer_before(uint32_t input) {
    pointer_released=false;
    if (!supported || !valid(input)) return;
    // Always let the normal pad mapper run. Native pointer ownership below is
    // selected by actual mouse activity, and ends on keyboard/controller input.
    vm_write8(input+0x5c,0);
    vm_write8(input+0x5d,1);
}
void haze_menu_pointer_after(uint32_t input) {
    if (!supported || !valid(input) || !has_menu(active_context) ||
        vm_read32(active_context+0x170)!=input) return;
    const auto pointer=haze_input_take_pointer();
    if (trace && (pointer.pressed || pointer.released || pointer.back || pointer.scroll))
        std::fprintf(stderr,"[menu-pointer] xy=%.5f,%.5f active=%d press=%d held=%d release=%d back=%d scroll=%d\n",
            pointer.x,pointer.y,pointer.active,pointer.pressed,pointer.held,pointer.released,pointer.back,pointer.scroll);
    if (!pointer.active || !std::isfinite(pointer.x) || !std::isfinite(pointer.y)) {
        pointer_owned=false;
        return;
    }
    vm_write8(input+0x5c,1);
    vm_write8(input+0x5d,0);
    write_float(input+0x14,pointer.x); write_float(input+0x18,pointer.y);
    if (pointer.pressed || !pointer_owned) {
        write_float(input+0x1c,pointer.x); write_float(input+0x20,pointer.y);
    }
    if (pointer.pressed) vm_write32(input+0x2c,0);
    if (pointer.held || pointer.released || pointer.back || pointer.scroll)
        vm_write32(input+0x60,0); // Match the guest pointer's activity timer.
    write_float(input+0x24,pointer.x); write_float(input+0x28,pointer.y);
    vm_write32(input+0x4c,pointer.released ? 2 : pointer.held ? 1 : 0);
    vm_write32(input+0x50,pointer.back ? 2 : 0);
    vm_write32(input+0x54,0);
    vm_write32(input+0x38,0);
    vm_write32(input+0x3c,0); vm_write32(input+0x40,0);
    if (pointer.scroll) {
        // Existing directional navigation scrolls the selected list and keeps
        // its focus visible. One detent is one navigation pulse.
        vm_write8(input+0x5c,0); vm_write8(input+0x5d,1);
        write_float(input+0x3c,pointer.scroll>0 ? 1.f : -1.f);
    }
    pointer_owned=true;
    pointer_released=pointer.released;
}

bool haze_menu_pointer_confirm(uint32_t input,uint32_t widget,uint32_t hit) {
    // The guest event dispatcher already resolved its topmost eligible widget.
    // Only that widget may receive a native mouse release, never a page-wide
    // keyboard action belonging to a different widget behind a dialog.
    return supported && pointer_owned && pointer_released && valid(widget) && widget==hit &&
        has_menu(active_context) && vm_read32(active_context+0x170)==input;
}
