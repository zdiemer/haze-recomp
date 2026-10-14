// Exercise the production camera hook against guest-memory fixtures. The
// original callback observes the values that will build this frame's matrices.
#include <cassert>
#include <cstring>
#include <unordered_map>
#include <cstdio>
#include "../port/haze_camera.cpp"
namespace {
std::unordered_map<uint64_t,uint32_t> memory;
void (*installed)(ppu_context*);
double dx,dy;
bool capture=true, inversion=false;
bool gameplay=true;
unsigned original_calls,consumptions;
float observed_yaw,observed_pitch;
constexpr uint32_t motor=0x1000,actor=0x2000,controls=0x3000;
}
uint8_t vm_read8(uint64_t a) { return static_cast<uint8_t>(memory[a]); }
uint32_t vm_read32(uint64_t a) { return memory[a]; }
void vm_write32(uint64_t a,uint32_t v) { memory[a]=v; }
extern "C" void ppu_register_function(uint64_t a,void (*fn)(ppu_context*)) {
    assert(a==0x24d3a0); installed=fn;
}
bool haze_input_take_mouse(double& x,double& y,double& speed,bool& invert) {
    x=dx; y=dy; dx=dy=0; speed=.1; invert=inversion; ++consumptions;
    return capture;
}
bool haze_menu_gameplay_allowed() { return gameplay; }
void func_0024D3A0(ppu_context*) {
    ++original_calls;
    observed_yaw=read_float(motor+0xb0); observed_pitch=read_float(motor+0xac);
}
static bool near_value(double a,double b) { return std::abs(a-b)<1e-5; }
int main() {
    haze_camera_initialize(); assert(!installed); // Fail closed on unknown ELF.
    memory[0x24d3a0]=0x7d800026; memory[0x24d3a4]=0xf821fed1;
    memory[0x24dd70]=0xd09f00ac; memory[0xde0c14]=0xe499f0;
    memory[0xe499f0]=0x24d3a0;
    haze_camera_initialize(); assert(installed);
    memory[motor+0x10]=actor; memory[actor+0x1a0]=controls;
    memory[controls]=0xdd9f78; memory[motor+0xa0]=1;
    write_float(motor+0x80,1.15f); write_float(motor+0x84,-1.25f);
    ppu_context ctx{}; ctx.gpr[3]=motor;
    auto reset=[] { write_float(motor+0xb0,0); write_float(motor+0xac,0); };
    reset(); dx=1; dy=1; installed(&ctx);
    assert(near_value(observed_yaw,-.00174532925) && near_value(observed_pitch,-.00174532925));
    installed(&ctx); assert(near_value(observed_yaw,-.00174532925)); // No residual motion.
    reset(); dx=900; installed(&ctx); const float fast=observed_yaw;
    assert(near_value(fast,-1.57079632679)); // 90-degree flick in one update, uncapped.
    reset(); for(int i=0;i<90;++i) { dx=10; installed(&ctx); }
    assert(near_value(fast,observed_yaw)); // Distance, regardless of packet/frame count.
    dy=10000; installed(&ctx); assert(near_value(observed_pitch,-1.25));
    inversion=true; dy=10000; installed(&ctx); assert(near_value(observed_pitch,1.15));
    reset(); capture=false; dx=900; installed(&ctx); assert(observed_yaw==0);
    capture=true; memory[controls]=0xdeadbeef; dx=900;
    const auto before=consumptions; installed(&ctx);
    assert(observed_yaw==0 && consumptions==before && dx==900); // AI cannot steal input.
    memory[controls]=0xdd9f78; memory[motor+0xa0]=0; installed(&ctx);
    assert(observed_yaw==0 && dx==0); // Inactive player discards pending counts.
    memory[motor+0xa0]=1; installed(&ctx); assert(observed_yaw==0);
    gameplay=false; dx=900; dy=900; installed(&ctx);
    assert(observed_yaw==0 && observed_pitch==0 && dx==0 && dy==0); // Both axes discarded, even if captured.
    gameplay=true; installed(&ctx); assert(observed_yaw==0 && observed_pitch==0);
    assert(original_calls==101);
    puts("Native camera: same-frame rotation, no deadzone/cap/drift, distance invariance, limits and gating passed.");
}
