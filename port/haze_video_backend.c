/* Compile the pinned SDK renderer once, extending it inside its owning TU so
 * cached descriptors and GPU resources never escape to the UI/config thread. */
#include <stdint.h>
static void haze_movie_capture_backbuffer(uint32_t backbuffer);
#include "haze_rsx_live_draw.c"
#include "haze_video_backend.h"

/* Movie uploads and guest UI draws share the command list. The compositor's
 * existing handoff lock waits for guest rendering before either transition. */
void haze_movie_render_init(void) {
    g_ld_movie_composite_ui=1;
}
static unsigned haze_movie_frames,haze_movie_sequence;
void haze_movie_render_mode(int on) {
    rsx_live_draw_set_movie_mode(on);
    if(on) { haze_movie_frames=0; ++haze_movie_sequence; }
    else { fprintf(stderr,"[movie] Renderer released after %u presented frames.\n",haze_movie_frames); fflush(stderr); }
}
void haze_movie_render_guest_flip(void) {
    // Haze reports flips through cellGcm's external notification rather than
    // the NV4097 flip method. Complete the compositor frame here too: it fences
    // guest draws before recycling their constant/descriptor upload rings.
    if(g.ready && g_ld_movie_mode) ld_movie_capture_overlay();
}
void haze_movie_render_frame(const uint8_t* rgba,uint32_t width,uint32_t height) {
    if(!g.ready) return;
    ++haze_movie_frames;
    rsx_live_draw_present_rgba(rgba,width,height);
    if(!g.ready) --haze_movie_frames;
}
static void haze_movie_capture_backbuffer(uint32_t backbuffer) {
    // Opt-in GPU readback proves that decoded frames reached the swap chain.
    // Disabled diagnostics perform no extra readback or file writes.
    const char* directory=getenv("HAZE_MOVIE_CAPTURE");
    if(directory && *directory && haze_movie_frames==60) {
        surface_t surface={0};
        surface.tex=g.backbuf[backbuffer]; surface.w=g_ld_resolution.host_w;
        surface.h=g_ld_resolution.host_h; surface.fmt=DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RESOURCE_BARRIER barrier={0}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=surface.tex;
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&barrier);
        char path[MAX_PATH*2]; snprintf(path,sizeof(path),"%s/movie_%u.ppm",directory,haze_movie_sequence);
        const u64 nonblack=ld_dump_surface_ppm(path,&surface);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&barrier); ld_flush(LD_FLUSH_READBACK);
        fprintf(stderr,"[movie] GPU capture %s nonblack=%llu\n",path,(unsigned long long)nonblack); fflush(stderr);
    }
}

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

int haze_video_display_get(int* requested,int* active,char* error,unsigned capacity) {
    BOOL fullscreen=FALSE;
    if (!requested || !active || !g.ready || haze_pending.active ||
        GetWindowThreadProcessId(g_ld_display_hwnd,NULL)!=GetCurrentThreadId())
        return haze_video_error(error,capacity,"Display changes are unavailable on this renderer.");
    if (FAILED(g.swap->lpVtbl->GetFullscreenState(g.swap,&fullscreen,NULL)))
        return haze_video_error(error,capacity,"Cannot read the current fullscreen state.");
    *requested=g_ld_fullscreen_requested; *active=fullscreen!=FALSE; return 1;
}
static HRESULT haze_display_buffers(void) {
    HRESULT result;
    for (u32 i=0;i<LD_SWAP_BUFFERS;++i) {
        if (g.backbuf[i]) g.backbuf[i]->lpVtbl->Release(g.backbuf[i]);
        g.backbuf[i]=NULL;
    }
    result=g.swap->lpVtbl->ResizeBuffers(g.swap,LD_SWAP_BUFFERS,
        g_ld_resolution.host_w,g_ld_resolution.host_h,DXGI_FORMAT_R8G8B8A8_UNORM,0);
    if (FAILED(result)) return result;
    for (u32 i=0;i<LD_SWAP_BUFFERS;++i) {
        result=g.swap->lpVtbl->GetBuffer(g.swap,i,&IID_ID3D12Resource,(void**)&g.backbuf[i]);
        if (FAILED(result)) return result;
        g.dev->lpVtbl->CreateRenderTargetView(g.dev,g.backbuf[i],NULL,rtv_handle(i));
    }
    return S_OK;
}
int haze_video_display_set(int requested,int active,char* error,unsigned capacity) {
    int previous_requested,previous_active;
    BOOL actual=FALSE;
    HRESULT result,observed;
    int buffers_touched=0;
    if ((requested!=0 && requested!=1) || (active!=0 && active!=1) || active>requested)
        return haze_video_error(error,capacity,"Invalid fullscreen request.");
    if (!haze_video_display_get(&previous_requested,&previous_active,error,capacity)) return 0;
    if (previous_active==active && g_ld_fullscreen_state==active) {
        g_ld_fullscreen_requested=requested; return 1;
    }
    ld_flush(LD_FLUSH_PRESENT);
    if (!g.ready || FAILED(g.dev->lpVtbl->GetDeviceRemovedReason(g.dev)))
        return haze_video_error(error,capacity,"The graphics device failed before the display change.");
    result=g.swap->lpVtbl->SetFullscreenState(g.swap,active,NULL);
    observed=g.swap->lpVtbl->GetFullscreenState(g.swap,&actual,NULL);
    /* S_OK is required: DXGI can report a successful-status transition still
     * in progress. Never advertise an unverified display mode as applied. */
    if (result==S_OK && SUCCEEDED(observed) && actual==active) {
        buffers_touched=1; result=haze_display_buffers();
    }
    else result=E_FAIL;
    if (SUCCEEDED(result)) {
        g_ld_fullscreen_requested=requested; g_ld_fullscreen_state=actual;
        g_ld_display_focused=GetForegroundWindow()==g_ld_display_hwnd && !IsIconic(g_ld_display_hwnd);
        return 1;
    }
    if (!buffers_touched && SUCCEEDED(observed) && actual==previous_active && g_ld_fullscreen_state==previous_active)
        return haze_video_error(error,capacity,"The requested display mode is unavailable. Previous display retained.");
    /* A failed resize/GetBuffer can have already released some references.
     * Recreate the original scanout buffers before resuming presentation. */
    result=g.swap->lpVtbl->SetFullscreenState(g.swap,previous_active,NULL);
    if (result==S_OK && SUCCEEDED(g.swap->lpVtbl->GetFullscreenState(g.swap,&actual,NULL)) &&
        actual==previous_active && SUCCEEDED(haze_display_buffers())) {
        g_ld_fullscreen_requested=previous_requested; g_ld_fullscreen_state=actual;
        return haze_video_error(error,capacity,"The requested display mode is unavailable. Previous display restored.");
    }
    /* Do not present through missing buffers or claim rollback after device
     * loss. Release exclusive ownership so the desktop remains reachable. */
    g.swap->lpVtbl->SetFullscreenState(g.swap,FALSE,NULL);
    g_ld_fullscreen_requested=0; g_ld_fullscreen_state=FALSE; g.ready=0;
    return haze_video_error(error,capacity,"The renderer could not restore the display. Restart Haze; settings were not saved.");
}
