/* Compile the pinned SDK renderer once, extending it inside its owning TU so
 * cached descriptors and GPU resources never escape to the UI/config thread. */
#include "rsx_live_draw.c"
#include "haze_video_backend.h"

static struct {
    int active, enable_postprocess;
    haze_video_live settings;
    rsx_postprocess created;
} haze_pending;

static int haze_video_error(char* error, unsigned capacity, const char* message) {
    if (capacity) snprintf(error, capacity, "%s", message);
    return 0;
}
int haze_video_prepare(const haze_video_live* s, char* error, unsigned capacity) {
    if (!s || s->aa>1 || (s->anisotropy!=1 && s->anisotropy!=2 &&
        s->anisotropy!=4 && s->anisotropy!=8 && s->anisotropy!=16) ||
        s->frame_limit>240 || s->vsync < -1 || s->vsync>1 ||
        !isfinite(s->mip_bias) || s->mip_bias < -2 || s->mip_bias>2 ||
        !isfinite(s->sharpening) || s->sharpening<0 || s->sharpening>1 ||
        !isfinite(s->exposure) || s->exposure < -2 || s->exposure>2)
        return haze_video_error(error,capacity,"Invalid video settings.");
    if (!g.ready || GetWindowThreadProcessId(g_ld_display_hwnd,NULL)!=GetCurrentThreadId())
        return haze_video_error(error,capacity,"Video settings are unavailable on this renderer.");
    if (haze_pending.active)
        return haze_video_error(error,capacity,"Another video change is in progress.");
    haze_pending.enable_postprocess=s->aa || s->sharpening || s->exposure;
    if (haze_pending.enable_postprocess && !g_ld_postprocess.output &&
        rsx_postprocess_init(&haze_pending.created,g.dev,
            g_ld_resolution.host_w,g_ld_resolution.host_h)!=0)
        return haze_video_error(error,capacity,"Could not allocate video effects. Previous settings retained.");
    /* Wait before invalidating CPU sampler descriptors or releasing resources
     * that a recorded/in-flight command list can still reference. */
    ld_flush(LD_FLUSH_PRESENT);
    if (!g.ready || FAILED(g.dev->lpVtbl->GetDeviceRemovedReason(g.dev))) {
        rsx_postprocess_destroy(&haze_pending.created);
        return haze_video_error(error,capacity,"The graphics device failed. Settings were not changed.");
    }
    haze_pending.settings=*s;
    haze_pending.active=1;
    return 1;
}
void haze_video_finish(int commit) {
    if (!haze_pending.active) return;
    if (commit) {
        const haze_video_live* s=&haze_pending.settings;
        if (g_ld_anisotropy!=s->anisotropy || g_ld_mip_bias!=s->mip_bias) {
            g.n_samplers=0;
            g.smp_ring_used=0;
            memset(g.smp_keys,0,sizeof g.smp_keys);
            memset(g.smp_ring_slots,0,sizeof g.smp_ring_slots);
        }
        g_ld_aa=s->aa; g_ld_anisotropy=s->anisotropy;
        g_ld_mip_bias=s->mip_bias; g_ld_sharpening=s->sharpening;
        g_ld_exposure=s->exposure; g_ld_vsync=s->vsync;
        if (haze_pending.created.output) {
            g_ld_postprocess=haze_pending.created;
            memset(&haze_pending.created,0,sizeof haze_pending.created);
        }
        if (!haze_pending.enable_postprocess) rsx_postprocess_destroy(&g_ld_postprocess);
        /* Keep the existing timer; only its deadline policy changes. */
        g_ld_frame_limiter.frequency=g_ld_qpc_frequency;
        g_ld_frame_limiter.period=s->frame_limit && g_ld_qpc_frequency>0 ?
            (g_ld_qpc_frequency+s->frame_limit-1)/s->frame_limit : 0;
        g_ld_frame_limiter.last_present=0;
    }
    rsx_postprocess_destroy(&haze_pending.created);
    memset(&haze_pending,0,sizeof haze_pending);
}
int haze_video_stop(unsigned timeout_ms) {
    if (GetWindowThreadProcessId(g_ld_display_hwnd,NULL)!=GetCurrentThreadId()) return 0;
    haze_video_finish(0);
    int ok=1;
    /* No more draws follow this call. A queue fence covers submitted work;
     * the unfinished command list is abandoned at process exit. Do not call
     * ld_flush here: its normal rendering wait is deliberately unbounded. */
    if (g.ready && g.queue && g.fence) {
        const u64 value=++g.fence_value;
        if (FAILED(g.queue->lpVtbl->Signal(g.queue,g.fence,value))) ok=0;
        else if (g.fence->lpVtbl->GetCompletedValue(g.fence)<value) {
            if (FAILED(g.fence->lpVtbl->SetEventOnCompletion(g.fence,value,g.fence_event)) ||
                WaitForSingleObject(g.fence_event,timeout_ms)!=WAIT_OBJECT_0) ok=0;
        }
    }
    if (g.swap && FAILED(g.swap->lpVtbl->SetFullscreenState(g.swap,FALSE,NULL))) ok=0;
    g_ld_fullscreen_requested=0;
    rsx_frame_limiter_destroy(&g_ld_frame_limiter);
    g.ready=0;
    fprintf(stderr,"[quit] Renderer stopped; final queue/display status=%s.\n",ok ? "complete" : "incomplete");
    return ok;
}
