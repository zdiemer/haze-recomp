// Exercise the guest actuator override against the SDK's return conventions.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include "../port/haze_pad_guest.cpp"
namespace {
std::unordered_map<uint64_t,uint8_t> memory;
void (*handler)(ppu_context*);
uint32_t registered;
s32 opened=s32(CELL_PAD_ERROR_NOT_OPENED);
unsigned requests;
u32 rumble_port;
u8 rumble_small,rumble_large;
}
uint8_t vm_read8(uint64_t a) { return memory[a]; }
extern "C" void ps3_hle_register_ctx(uint32_t nid,const char* name,void (*fn)(ppu_context*)) {
    assert(!std::strcmp(name,"cellPadSetActDirect")); registered=nid; handler=fn;
}
extern "C" s32 cellPadGetCapabilityInfo(u32,CellPadCapabilityInfo* info) { assert(!info); return opened; }
void haze_input_rumble(u32 port,u8 small,u8 large) { ++requests; rumble_port=port; rumble_small=small; rumble_large=large; }
static s32 call(uint32_t port,uint32_t param) {
    ppu_context ctx{}; ctx.gpr[3]=port; ctx.gpr[4]=param;
    handler(&ctx);
    assert(ctx.gpr[3]==uint64_t(int64_t(s32(uint32_t(ctx.gpr[3])))));
    return s32(uint32_t(ctx.gpr[3]));
}
int main() {
    haze_pad_guest_register();
    assert(registered==0xF65544EEu && handler); // SHA-1 NID the generated table uses.
    memory[0x1000]=1; memory[0x1001]=200;
    assert(call(0,0x1000)==s32(CELL_PAD_ERROR_NOT_OPENED) && !requests);
    opened=s32(CELL_PAD_ERROR_INVALID_PARAMETER);
    assert(call(0,0)==s32(CELL_PAD_ERROR_INVALID_PARAMETER) && !requests);
    assert(call(CELL_PAD_MAX_PORT_NUM,0x1000)==s32(CELL_PAD_ERROR_INVALID_PARAMETER) && !requests);
    assert(call(0,0x1000)==CELL_OK && requests==1 && !rumble_port && rumble_small==1 && rumble_large==200);
    memory[0x1000]=0; memory[0x1001]=0;
    assert(call(3,0x1000)==CELL_OK && requests==2 && rumble_port==3 && !rumble_small && !rumble_large);
    puts("Guest actuator validation, motor order and HLE override registration passed.");
}
