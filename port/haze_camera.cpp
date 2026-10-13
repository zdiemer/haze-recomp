#include "ppu_recomp.h"
#include "haze_camera.h"
#include "haze_input.h"
#include "haze_mouse.h"
#include "haze_menu.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" void ppu_register_function(uint64_t,void (*)(ppu_context*));
namespace {
float read_float(uint32_t address) {
    uint32_t bits=vm_read32(address); float value;
    std::memcpy(&value,&bits,sizeof(value)); return value;
}
void write_float(uint32_t address,float value) {
    uint32_t bits; std::memcpy(&bits,&value,sizeof(bits)); vm_write32(address,bits);
}
void update_look(ppu_context* ctx) {
    static const bool trace=std::getenv("HAZE_MOUSE_TRACE")!=nullptr;
    bool moved=false;
    float before_yaw=0,before_pitch=0;
    double mouse_x=0,mouse_y=0;
    const uint32_t motor=static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t actor=vm_read32(motor+0x10);
    // Only a player with the local G5 controls component may consume the mouse.
    // AI and remote characters use this motor too.
    const uint32_t controls=actor ? vm_read32(actor+0x1a0) : 0;
    if (controls && vm_read32(controls)==0x00dd9f78) {
        double x=0,y=0,speed=0; bool invert=false;
        if (haze_input_take_mouse(x,y,speed,invert) &&
            haze_menu_gameplay_allowed() && vm_read8(motor+0xa0)) {
            const float yaw=read_float(motor+0xb0),pitch=read_float(motor+0xac);
            const float upper=read_float(motor+0x80),lower=read_float(motor+0x84);
            if (std::isfinite(yaw) && std::isfinite(pitch) &&
                std::isfinite(lower) && std::isfinite(upper) && lower<=upper) {
                const auto look=haze_mouse_look(yaw,pitch,x,y,speed,invert,lower,upper);
                moved=x!=0 || y!=0;
                before_yaw=yaw+read_float(motor+0xb4); before_pitch=pitch;
                mouse_x=x; mouse_y=y;
                write_float(motor+0xb0,look.yaw);
                write_float(motor+0xac,look.pitch);
                // Suppress stick input and its angular velocity history. WASD,
                // recoil, animation, collision and camera limits still run.
                vm_write32(motor+0x14,0); vm_write32(motor+0x18,0);
                vm_write32(motor+0xc8,0); vm_write32(motor+0xcc,0);
            }
        }
    }
    func_0024D3A0(ctx);
    if (trace && moved)
        std::fprintf(stderr,"[mouse] counts=%.0f,%.0f yaw=%.8f->%.8f pitch=%.8f->%.8f motor=%08X\n",
            mouse_x,mouse_y,before_yaw,read_float(motor+0xb0)+read_float(motor+0xb4),
            before_pitch,read_float(motor+0xac),motor);
}
}
void haze_camera_initialize() {
    // BLUS30094 01.00: virtual character-look update, BEFORE camera/aim matrices
    // are built. Direct calls to this function are absent in this title's lift.
    // Refuse a different layout instead of writing guessed object offsets.
    if (vm_read32(0x24d3a0)!=0x7d800026 ||
        vm_read32(0x24d3a4)!=0xf821fed1 ||
        vm_read32(0x24dd70)!=0xd09f00ac ||
        vm_read32(0xde0c14)!=0xe499f0 || vm_read32(0xe499f0)!=0x24d3a0) {
        std::fprintf(stderr,"[input] Native mouse camera hook unavailable: unsupported Haze layout\n");
        return;
    }
    ppu_register_function(0x24d3a0,update_look);
    std::fprintf(stderr,"[input] Native mouse camera hook installed (distance-based rotation)\n");
}
