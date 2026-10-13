#include "ppu_recomp.h"
extern "C" int haze_sail_movie_ended(uint32_t player);

// Audited BLUS30094 func_00718880 normally detects EOF when GetFrame2 stops
// returning guest texture buffers. Native frames go directly to the renderer;
// publish the same movie-ended flag without fabricating guest texture pointers.
void haze_movie_guest_poll(ppu_context* ctx) {
    const uint32_t table=vm_read32(ctx->gpr[2]-0x7c38);
    const uint32_t player=vm_read32(table-0x7fac);
    if(haze_sail_movie_ended(player))
        vm_write32(vm_read32(table-0x7fb0),1);
}
