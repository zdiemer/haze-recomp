/* Deterministic GPU seam for the actual title-owned transaction, not a second
 * transaction implementation. Operations record resource/fence ordering. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef unsigned u32;
typedef unsigned long long u64;
struct fake_fence;
struct fake_queue;
struct fake_swap;
typedef struct { HRESULT (*Signal)(struct fake_queue*,struct fake_fence*,u64); } queue_methods;
typedef struct { u64 (*GetCompletedValue)(struct fake_fence*); HRESULT (*SetEventOnCompletion)(struct fake_fence*,u64,HANDLE); } fence_methods;
typedef struct { HRESULT (*SetFullscreenState)(struct fake_swap*,BOOL,void*); } swap_methods;
typedef struct fake_queue { queue_methods* lpVtbl; } fake_queue;
typedef struct fake_fence { fence_methods* lpVtbl; } fake_fence;
typedef struct fake_swap { swap_methods* lpVtbl; } fake_swap;
static int signal_failure,event_failure,restore_failure,restored_display,timer_destroyed;
static u64 completed_value;
static DWORD wait_result=WAIT_OBJECT_0,wait_limit;
static HRESULT queue_signal(fake_queue*,fake_fence*,u64) { return signal_failure ? E_FAIL : S_OK; }
static u64 fence_completed(fake_fence*) { return completed_value; }
static HRESULT fence_event(fake_fence*,u64,HANDLE) { return event_failure ? E_FAIL : S_OK; }
static HRESULT swap_restore(fake_swap*,BOOL fullscreen,void*) { if (!fullscreen) ++restored_display; return restore_failure ? E_FAIL : S_OK; }
static DWORD bounded_wait(HANDLE,DWORD timeout) { wait_limit=timeout; return wait_result; }
#define WaitForSingleObject bounded_wait
static queue_methods queue_vtable={queue_signal}; static fake_queue queue={&queue_vtable};
static fence_methods fence_vtable={fence_completed,fence_event}; static fake_fence fence={&fence_vtable};
static swap_methods swap_vtable={swap_restore}; static fake_swap swap={&swap_vtable};
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
    unsigned long long smp_keys[4]; unsigned smp_ring_slots[4][4];
    fake_queue* queue; fake_fence* fence; fake_swap* swap; u64 fence_value; HANDLE fence_event; } g;
static HWND g_ld_display_hwnd;
static unsigned g_ld_aa,g_ld_anisotropy;
static float g_ld_mip_bias,g_ld_sharpening,g_ld_exposure;
static int g_ld_vsync,g_ld_fullscreen_requested;
static rsx_postprocess g_ld_postprocess;
static struct { unsigned host_w,host_h; } g_ld_resolution;
static LONGLONG g_ld_qpc_frequency=1000000;
typedef struct { LONGLONG frequency,period,last_present; } fake_limiter;
static fake_limiter g_ld_frame_limiter;
static void rsx_frame_limiter_destroy(fake_limiter*) { ++timer_destroyed; }
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
