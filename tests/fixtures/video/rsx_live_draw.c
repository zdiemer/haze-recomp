/* Deterministic GPU seam for the actual title-owned transaction, not a second
 * transaction implementation. Operations record resource/fence ordering. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef unsigned u32;
typedef struct { void* output; } rsx_postprocess;
static int allocation_failure, flush_failure, removed, allocated, released, fences;
static DWORD owner_thread;
static DWORD fake_window_thread(HWND window, DWORD* pid) {
    (void)window; (void)pid; return owner_thread;
}
#define GetWindowThreadProcessId fake_window_thread
struct fake_device;
typedef struct { HRESULT (*GetDeviceRemovedReason)(struct fake_device*); } fake_vtable;
typedef struct fake_device { fake_vtable* lpVtbl; } fake_device;
static HRESULT removal_reason(fake_device* device) { (void)device; return removed ? E_FAIL : S_OK; }
static fake_vtable methods={removal_reason};
static fake_device device={&methods};
static struct { int ready; fake_device* dev; unsigned n_samplers,smp_ring_used;
    unsigned long long smp_keys[4]; unsigned smp_ring_slots[4][4]; } g;
static HWND g_ld_display_hwnd;
static unsigned g_ld_aa,g_ld_anisotropy;
static float g_ld_mip_bias,g_ld_sharpening,g_ld_exposure;
static int g_ld_vsync;
static rsx_postprocess g_ld_postprocess;
static struct { unsigned host_w,host_h; } g_ld_resolution;
static LONGLONG g_ld_qpc_frequency=1000000;
static struct { LONGLONG frequency,period,last_present; } g_ld_frame_limiter;
enum { LD_FLUSH_PRESENT };
static int rsx_postprocess_init(rsx_postprocess* p,fake_device* d,unsigned w,unsigned h) {
    (void)d; (void)w; (void)h;
    if (allocation_failure) return -1;
    p->output=(void*)1; ++allocated; return 0;
}
static void rsx_postprocess_destroy(rsx_postprocess* p) {
    if (p->output) ++released; p->output=NULL;
}
static void ld_flush(int reason) { (void)reason; ++fences; if (flush_failure) g.ready=0; }
