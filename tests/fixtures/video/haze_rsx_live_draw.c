/* The port now wraps the audited renderer copy under this name. */
#include "rsx_live_draw.c"
/* Movie operations are outside this settings transaction test. */
static int g_ld_movie_composite_ui, g_ld_movie_mode;
static void rsx_live_draw_set_movie_mode(int on) { g_ld_movie_mode=on; }
static void ld_movie_capture_overlay() {}
static void rsx_live_draw_present_rgba(const uint8_t*,uint32_t,uint32_t) {}
typedef struct { fake_resource* tex; unsigned w,h,fmt; } surface_t;
enum { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, LD_FLUSH_READBACK=2 };
typedef struct { int Type; struct { fake_resource* pResource; int Subresource,StateBefore,StateAfter; } Transition; } D3D12_RESOURCE_BARRIER;
static u64 ld_dump_surface_ppm(const char*,const surface_t*) { return 0; }
