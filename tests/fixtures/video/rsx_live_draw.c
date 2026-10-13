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
struct fake_resource;
typedef struct { HRESULT (*GetDeviceRemovedReason)(struct fake_device*);
    void (*CreateRenderTargetView)(struct fake_device*,struct fake_resource*,void*,unsigned); } fake_vtable;
typedef struct fake_device { fake_vtable* lpVtbl; } fake_device;
static HRESULT removal_reason(fake_device* device) { (void)device; return removed ? E_FAIL : S_OK; }
static unsigned views,buffer_releases,resize_calls,get_buffer_calls,set_fullscreen_calls;
static unsigned fail_resize_call,fail_buffer_call,fail_fullscreen_call;
static int all_resize_fail,actual_fullscreen;
static void create_view(fake_device* dev,struct fake_resource* resource,void* desc,unsigned handle) {
    (void)dev; (void)resource; (void)desc; (void)handle; ++views;
}
static fake_vtable methods={removal_reason,create_view};
static fake_device device={&methods};
typedef struct { void (*Release)(struct fake_resource*); } resource_methods;
typedef struct fake_resource { resource_methods* lpVtbl; } fake_resource;
static void release_buffer(fake_resource* resource) { (void)resource; ++buffer_releases; }
static resource_methods resource_vtable={release_buffer};
static fake_resource buffers[2]={{&resource_vtable},{&resource_vtable}};
struct fake_swap;
typedef struct {
    HRESULT (*GetFullscreenState)(struct fake_swap*,BOOL*,void*);
    HRESULT (*SetFullscreenState)(struct fake_swap*,BOOL,void*);
    HRESULT (*ResizeBuffers)(struct fake_swap*,unsigned,unsigned,unsigned,unsigned,unsigned);
    HRESULT (*GetBuffer)(struct fake_swap*,unsigned,const void*,void**);
} swap_methods;
typedef struct fake_swap { swap_methods* lpVtbl; } fake_swap;
static HRESULT get_fullscreen(fake_swap* swap,BOOL* active,void* output) { (void)swap; (void)output; *active=actual_fullscreen; return S_OK; }
static HRESULT set_fullscreen(fake_swap* swap,BOOL active,void* output) {
    (void)swap; (void)output;
    if (++set_fullscreen_calls==fail_fullscreen_call) return E_FAIL;
    actual_fullscreen=active; return S_OK;
}
static HRESULT resize_buffers(fake_swap* swap,unsigned count,unsigned width,unsigned height,unsigned format,unsigned flags) {
    (void)swap; (void)count; (void)width; (void)height; (void)format; (void)flags;
    return (++resize_calls==fail_resize_call || all_resize_fail) ? E_FAIL : S_OK;
}
static HRESULT get_buffer(fake_swap* swap,unsigned index,const void* iid,void** resource) {
    (void)swap; (void)iid;
    if (++get_buffer_calls==fail_buffer_call) return E_FAIL;
    *resource=&buffers[index]; return S_OK;
}
static swap_methods swap_vtable={get_fullscreen,set_fullscreen,resize_buffers,get_buffer};
static fake_swap swap_chain={&swap_vtable};
enum { LD_SWAP_BUFFERS=2,DXGI_FORMAT_R8G8B8A8_UNORM=28 };
static const unsigned IID_ID3D12Resource=1;
static unsigned rtv_handle(unsigned index) { return index; }
static struct { int ready; fake_device* dev; unsigned n_samplers,smp_ring_used;
    unsigned long long smp_keys[4]; unsigned smp_ring_slots[4][4];
    fake_swap* swap; fake_resource* backbuf[2]; } g;
static HWND g_ld_display_hwnd;
static int g_ld_fullscreen_requested,g_ld_fullscreen_state,g_ld_display_focused;
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
