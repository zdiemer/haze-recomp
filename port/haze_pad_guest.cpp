#include "ppu_recomp.h"
#include "haze_input.h"

extern "C" void ps3_hle_register_ctx(uint32_t,const char*,void (*)(ppu_context*));
namespace {
// Replaces the SDK's XInput actuator path. Context handlers dispatch before the
// generated NID table, so the selected SDL device owns feedback instead.
void set_act_direct(ppu_context* ctx) {
    const auto port=uint32_t(ctx->gpr[3]),param=uint32_t(ctx->gpr[4]);
    // The SDK checks NOT_OPENED before its arguments; a null info writes nothing.
    constexpr auto invalid=s32(CELL_PAD_ERROR_INVALID_PARAMETER);
    s32 result=cellPadGetCapabilityInfo(0,nullptr);
    if (result==invalid) {
        result=port<CELL_PAD_MAX_PORT_NUM && param ? CELL_OK : invalid;
        if (result==CELL_OK)
            haze_input_rumble(port,vm_read8(param+CELL_PAD_ACTUATOR_PARAM_SMALL),vm_read8(param+CELL_PAD_ACTUATOR_PARAM_LARGE));
    }
    ctx->gpr[3]=uint64_t(int64_t(result));
}
}
void haze_pad_guest_register() { ps3_hle_register_ctx(0xF65544EEu,"cellPadSetActDirect",set_act_direct); }
