#include "ppu_recomp.h"
#include "haze_player_profile.h"

extern "C" void ps3_hle_register_ctx(uint32_t,const char*,void(*)(ppu_context*));
extern "C" int32_t cellSysutilGetSystemParamString(int32_t,uint32_t,uint32_t);
extern "C" int32_t sceNpManagerGetNpId(void*);

namespace {
void system_name(ppu_context* ctx) {
    const auto id=uint32_t(ctx->gpr[3]), address=uint32_t(ctx->gpr[4]), size=uint32_t(ctx->gpr[5]);
    // Retain SDK validation and behavior for every other system parameter.
    const auto result=cellSysutilGetSystemParamString(int32_t(id),address,size);
    // 0x131 feeds Haze's internal profile key (004EA250 -> 004D3228).
    // Keep that stable; the separate display field is refreshed below.
    if (!result && id==0x113) {
        const auto name=haze_player_name();
        uint32_t i=0;
        for (;i+1<size && i<name.size();++i) vm_write8(address+i,uint8_t(name[i]));
        vm_write8(address+i,0);
    }
    ctx->gpr[3]=uint64_t(int64_t(result));
}
void online_name(ppu_context* ctx) {
    const auto address=uint32_t(ctx->gpr[3]);
    const auto result=sceNpManagerGetNpId(reinterpret_cast<void*>(uintptr_t(address)));
    if (!result) {
        const auto name=haze_player_name();
        // SceNpOnlineId is the first 20 bytes of SceNpId. Retain the SDK's
        // remaining identity fields and all initialization/offline errors.
        for (unsigned i=0;i<17;++i) vm_write8(address+i,i<name.size() ? uint8_t(name[i]):0);
    }
    ctx->gpr[3]=uint64_t(int64_t(result));
}
}
void haze_player_profile_register() {
    ps3_hle_register_ctx(0x938013a0,"Haze profile system name",system_name);
    ps3_hle_register_ctx(0xfe37a7f4,"Haze profile online name",online_name);
}

void haze_player_profile_tick() {
    // Audited BLUS30094 setter 004D31F8 copies a display name to profile+4d.
    // 004D3228 writes the separate key at +d; do not rename that key or the
    // savedata directory. The header and LAN player setup use the display name.
    constexpr uint32_t code[]={0x3863004d,0x7c0802a6,0x38a00040,0x78630020};
    for (unsigned i=0;i<4;++i) if (vm_read32(0x4d31f8+i*4)!=code[i]) return;
    const auto valid=[](uint32_t p) { return p>=0x10000 && p<0xbffff000 && !(p&3); };
    const auto manager=vm_read32(0xf48de0);
    if (!valid(manager)) return;
    // 004E6F20(manager, 0): first local profile; other split-screen profiles
    // retain their own display names.
    const auto profile=vm_read32(manager+0x10);
    if (!valid(profile)) return;
    const auto name=haze_player_name();
    bool same=true;
    for (unsigned i=0;i<=name.size();++i)
        if (vm_read8(profile+0x4d+i)!=(i<name.size() ? uint8_t(name[i]):0)) same=false;
    if (!same) for (unsigned i=0;i<=name.size();++i)
        vm_write8(profile+0x4d+i,i<name.size() ? uint8_t(name[i]):0);
}
