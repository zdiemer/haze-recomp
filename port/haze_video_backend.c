/* Compile the pinned SDK renderer once, extending it inside its owning TU so
 * cached descriptors and GPU resources never escape to the UI/config thread. */
#include <stdint.h>
static void haze_movie_capture_backbuffer(uint32_t backbuffer);
static void haze_overlay_draw(uint32_t backbuffer);
static void haze_overlay_start(void);
static void haze_overlay_destroy(void);
static void ld_submit_frame(void);
static void ld_slots_shutdown(void);
static void haze_fingerprint_method(uint32_t method,uint32_t arg);
#include "haze_rsx_live_draw.c"
#include "haze_video_backend.h"

/* RSX_FRAME_FINGERPRINT_CSV=<path>: per presented guest frame, hash the 3D
 * stream's transform constants and count its draws. Constants carry every
 * visible object's matrices, so deterministic benchmark replays whose
 * simulations agree produce identical hashes; memory offsets are excluded
 * because they depend on allocation history. The renderer thread drains the
 * FIFO up to each completed flip before presenting it, so one row is one
 * guest frame in stream order. Written only at normal shutdown. */
typedef struct { uint64_t hash; uint32_t flips,constants,draws,pad; } haze_fingerprint_row;
#define HAZE_FINGERPRINT_CAP (1u<<18)
static int haze_fingerprint_enabled=-1;
static haze_fingerprint_row* haze_fingerprint_rows;
static uint64_t haze_fingerprint_total,haze_fingerprint_hash=0xCBF29CE484222325ull;
static uint32_t haze_fingerprint_constants,haze_fingerprint_draws;
/* Bounded constant trace for divergence diagnosis: the words of the frames
 * whose flip counts fall in [first, first+count), requested by the benchmark
 * clock at gameplay frame 0 and written with the fingerprint. */
typedef struct { uint32_t frame,draw,lane,value; } haze_trace_entry;
#define HAZE_TRACE_FRAMES 60u
#define HAZE_TRACE_CAP (1u<<21)
static volatile long haze_trace_first=-1;
static uint32_t haze_trace_load;
static haze_trace_entry* volatile haze_trace;
static uint32_t haze_trace_used,haze_trace_frame_start,haze_trace_kept;
void haze_fingerprint_trace_from(uint32_t flips) {
    if(haze_fingerprint_enabled<=0 || haze_trace_first>=0) return;
    haze_trace_entry* buffer=(haze_trace_entry*)calloc(HAZE_TRACE_CAP,sizeof(*buffer));
    if(!buffer) return;
    haze_trace_first=(long)flips;
    MemoryBarrier();
    haze_trace=buffer;
}
static void haze_trace_word(uint32_t lane,uint32_t value) {
    if(!haze_trace || haze_trace_kept>=HAZE_TRACE_FRAMES || haze_trace_used>=HAZE_TRACE_CAP) return;
    haze_trace_entry* e=&haze_trace[haze_trace_used++];
    e->frame=haze_trace_kept; e->draw=haze_fingerprint_draws; e->lane=lane; e->value=value;
}
static void haze_trace_frame(uint32_t flips) {
    if(!haze_trace || haze_trace_kept>=HAZE_TRACE_FRAMES) return;
    if((long)flips>=haze_trace_first && (long)flips<haze_trace_first+(long)HAZE_TRACE_FRAMES) {
        haze_trace_kept++; haze_trace_frame_start=haze_trace_used;
    } else haze_trace_used=haze_trace_frame_start;
}
static void haze_fingerprint_method(uint32_t method,uint32_t arg) {
    if(haze_fingerprint_enabled<0) {
        const char* path=getenv("RSX_FRAME_FINGERPRINT_CSV");
        haze_fingerprint_rows=path && *path ? (haze_fingerprint_row*)
            calloc(HAZE_FINGERPRINT_CAP,sizeof(*haze_fingerprint_rows)) : NULL;
        haze_fingerprint_enabled=haze_fingerprint_rows!=NULL;
    }
    if(!haze_fingerprint_enabled) return;
    const uint32_t canonical=method&0x1FFCu;
    if(canonical==0x1EFCu || (canonical>=0x1F00u && canonical<0x1F80u)) {
        haze_fingerprint_hash=(haze_fingerprint_hash^((uint64_t)canonical<<32|arg))*0x100000001B3ull;
        ++haze_fingerprint_constants;
        if(canonical==0x1EFCu) haze_trace_load=arg;
        else haze_trace_word(haze_trace_load*4u+((canonical-0x1F00u)>>2),arg);
    } else if(canonical==0x1808u && arg) ++haze_fingerprint_draws;
}
void haze_fingerprint_frame(uint32_t flips) {
    if(haze_fingerprint_enabled<=0) return;
    haze_trace_frame(flips);
    haze_fingerprint_row* row=&haze_fingerprint_rows[haze_fingerprint_total++&(HAZE_FINGERPRINT_CAP-1u)];
    row->hash=haze_fingerprint_hash; row->flips=flips;
    row->constants=haze_fingerprint_constants; row->draws=haze_fingerprint_draws;
    haze_fingerprint_hash=0xCBF29CE484222325ull;
    haze_fingerprint_constants=haze_fingerprint_draws=0;
}
void haze_fingerprint_dump(void) {
    const char* path=getenv("RSX_FRAME_FINGERPRINT_CSV");
    if(haze_fingerprint_enabled<=0 || !path) return;
    FILE* file=fopen(path,"wb");
    if(!file) { fprintf(stderr,"[frame-fingerprint] cannot open %s\n",path); return; }
    fprintf(file,"frame,flips,hash,constants,draws\n");
    const uint64_t first=haze_fingerprint_total>HAZE_FINGERPRINT_CAP ?
        haze_fingerprint_total-HAZE_FINGERPRINT_CAP : 0;
    for(uint64_t i=first;i<haze_fingerprint_total;++i) {
        const haze_fingerprint_row* row=&haze_fingerprint_rows[i&(HAZE_FINGERPRINT_CAP-1u)];
        fprintf(file,"%llu,%u,%016llX,%u,%u\n",(unsigned long long)i,row->flips,
            (unsigned long long)row->hash,row->constants,row->draws);
    }
    fclose(file);
    fprintf(stderr,"[frame-fingerprint] wrote %llu frames\n",(unsigned long long)(haze_fingerprint_total-first));
    if(haze_trace && haze_trace_kept) {
        char trace_path[1100];
        snprintf(trace_path,sizeof trace_path,"%s.trace.csv",path);
        if((file=fopen(trace_path,"wb"))!=NULL) {
            fprintf(file,"first_flips=%ld\nframe,draw,lane,value\n",haze_trace_first);
            for(uint32_t i=0;i<haze_trace_frame_start;++i)
                fprintf(file,"%u,%u,%u,%08X\n",haze_trace[i].frame,haze_trace[i].draw,
                    haze_trace[i].lane,haze_trace[i].value);
            fclose(file);
        }
    }
}

#ifndef HAZE_VIDEO_BACKEND_TEST
#include "haze_submission_slots.h"
#include "haze_overlay_d3d12.h"
#endif

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
        s->frame_limit>240 || s->vsync < -1 || s->vsync>1 || s->overlay>2 || s->overlay_position>3 ||
        !isfinite(s->mip_bias) || s->mip_bias < -2 || s->mip_bias>2 ||
        !isfinite(s->sharpening) || s->sharpening<0 || s->sharpening>1 ||
        !isfinite(s->exposure) || s->exposure < -2 || s->exposure>2)
        return haze_video_error(error,capacity,"Invalid video settings.");
    if (!g.ready || GetWindowThreadProcessId(g_ld_display_hwnd,NULL)!=GetCurrentThreadId())
        return haze_video_error(error,capacity,"Video settings are unavailable on this renderer.");
    if (haze_pending.active)
        return haze_video_error(error,capacity,"Another video change is in progress.");
    if (s->overlay && !haze_overlay_pipeline)
        return haze_video_error(error,capacity,"Performance overlay is unavailable on this graphics device.");
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
            /* prepare fenced all prior descriptor references. Sampler slots
             * are about to change identity; discard every page's host cache. */
            ld_sampler_pages_reset();
            g.n_samplers=0;
            g.smp_ring_used=0;
            memset(g.smp_keys,0,sizeof g.smp_keys);
            memset(g.smp_used,0,sizeof g.smp_used);
            memset(g.smp_ring_slots,0,sizeof g.smp_ring_slots);
        }
        g_ld_aa=s->aa; g_ld_anisotropy=s->anisotropy;
        g_ld_mip_bias=s->mip_bias; g_ld_sharpening=s->sharpening;
        g_ld_exposure=s->exposure; g_ld_vsync=s->vsync;
        haze_overlay_set(s->overlay,s->overlay_position);
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
    const ULONGLONG began=GetTickCount64();
    haze_video_finish(0);
    /* Start recipe persistence before the bundle write, then join both within
     * the existing display shutdown budget. Each writer owns detached data. */
    rsx_live_draw_finish_pipeline_recipes(0);
    ULONGLONG elapsed=GetTickCount64()-began;
    rsx_live_draw_finish_shader_bundle(elapsed>=timeout_ms ? 0 : timeout_ms-(DWORD)elapsed);
    elapsed=GetTickCount64()-began;
    rsx_live_draw_finish_pipeline_recipes(elapsed>=timeout_ms ? 0 : timeout_ms-(DWORD)elapsed);
    elapsed=GetTickCount64()-began;
    const DWORD remaining=elapsed>=timeout_ms ? 0 : timeout_ms-(DWORD)elapsed;
    int ok=1;
    /* No more draws follow this call. A queue fence covers submitted work;
     * the unfinished command list is abandoned at process exit. Do not call
     * ld_flush here: its normal rendering wait is deliberately unbounded. */
    if (g.ready && g.queue && g.fence) {
        const u64 value=++g.fence_value;
        if (FAILED(g.queue->lpVtbl->Signal(g.queue,g.fence,value))) ok=0;
        else if (g.fence->lpVtbl->GetCompletedValue(g.fence)<value) {
            if (FAILED(g.fence->lpVtbl->SetEventOnCompletion(g.fence,value,g.fence_event)) ||
                WaitForSingleObject(g.fence_event,remaining)!=WAIT_OBJECT_0) ok=0;
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
