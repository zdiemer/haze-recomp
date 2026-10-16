#include <cassert>
#include <cstdint>
#define HAZE_VIDEO_BACKEND_TEST
static bool haze_overlay_pipeline=true;
static unsigned overlay_mode,overlay_corner;
static void haze_overlay_set(unsigned mode,unsigned corner) { overlay_mode=mode; overlay_corner=corner; }
#include "../port/haze_video_backend.c"
int main() {
    g.dev=&device; g.ready=1; owner_thread=GetCurrentThreadId();
    g.n_samplers=3; g.smp_ring_used=2; g.smp_keys[0]=42;
    g_ld_anisotropy=1;
    haze_video_live high={1,16,60,0,.15f,0,-1}; char error[200];
    high.overlay=2; high.overlay_position=3;
    auto bad_overlay=high; bad_overlay.overlay=3;
    assert(!haze_video_prepare(&bad_overlay,error,sizeof error));
    bad_overlay=high; bad_overlay.overlay_position=4;
    assert(!haze_video_prepare(&bad_overlay,error,sizeof error));
    haze_overlay_pipeline=false;
    assert(!haze_video_prepare(&high,error,sizeof error));
    haze_overlay_pipeline=true;
    allocation_failure=1;
    assert(!haze_video_prepare(&high,error,sizeof error));
    assert(fences==0 && !g_ld_postprocess.output && g_ld_anisotropy==1 && g.n_samplers==3);
    allocation_failure=0;
    assert(haze_video_prepare(&high,error,sizeof error));
    assert(fences==1 && allocated==1 && !g_ld_postprocess.output && g_ld_anisotropy==1);
    assert(!haze_video_prepare(&high,error,sizeof error)); // No overlapping transactions.
    haze_video_finish(0);
    assert(overlay_mode==0 && overlay_corner==0);
    assert(released==1 && g.n_samplers==3 && g_ld_anisotropy==1);
    assert(haze_video_prepare(&high,error,sizeof error)); haze_video_finish(1);
    assert(overlay_mode==2 && overlay_corner==3);
    assert(g_ld_postprocess.output && g_ld_anisotropy==16 && !g.n_samplers && !g.smp_ring_used && !g.smp_keys[0]);
    assert(sampler_pages_reset_calls==1);
    assert(g_ld_frame_limiter.period==16667 && g_ld_frame_limiter.last_present==0);
    // Same allocation supports changes to effect parameters without recreation.
    int count=allocated; high.exposure=1;
    assert(haze_video_prepare(&high,error,sizeof error)); haze_video_finish(1);
    assert(allocated==count && g_ld_exposure==1);
    haze_video_live low={0,1,0,0,0,0,0};
    assert(haze_video_prepare(&low,error,sizeof error));
    assert(g_ld_postprocess.output); // Old resources survive until commit.
    haze_video_finish(1);
    assert(!g_ld_postprocess.output && g_ld_frame_limiter.period==0 && g_ld_anisotropy==1);
    flush_failure=1;
    count=released;
    assert(!haze_video_prepare(&high,error,sizeof error));
    assert(released==count+1 && !g_ld_postprocess.output && g_ld_anisotropy==1);
    flush_failure=0; g.ready=1; removed=1;
    assert(!haze_video_prepare(&high,error,sizeof error));
    assert(!g_ld_postprocess.output && g_ld_anisotropy==1);
    removed=0; owner_thread=0;
    count=allocated; assert(!haze_video_prepare(&high,error,sizeof error)); assert(allocated==count);
    owner_thread=GetCurrentThreadId(); high.sharpening=NAN;
    assert(!haze_video_prepare(&high,error,sizeof error)); assert(allocated==count);
    g.swap=&swap_chain; g.backbuf[0]=&buffers[0]; g.backbuf[1]=&buffers[1];
    int requested=-1,active=-1;
    assert(haze_video_display_get(&requested,&active,error,sizeof error) && !requested && !active);
    int before_fences=fences;
    assert(haze_video_display_set(1,1,error,sizeof error));
    assert(actual_fullscreen && g_ld_fullscreen_requested && g_ld_fullscreen_state && fences==before_fences+1);
    assert(resize_calls==1 && views==2 && buffer_releases==2);
    assert(haze_video_display_set(1,1,error,sizeof error) && resize_calls==1);
    assert(haze_video_display_set(0,0,error,sizeof error) && !actual_fullscreen && !g_ld_fullscreen_requested);
    const auto stable_views=views;
    fail_fullscreen_call=set_fullscreen_calls+1;
    assert(!haze_video_display_set(1,1,error,sizeof error) && !actual_fullscreen && !g_ld_fullscreen_requested && g.ready);
    assert(g.backbuf[0] && g.backbuf[1] && views==stable_views); // denied mode does not discard usable buffers
    fail_resize_call=resize_calls+1;
    assert(!haze_video_display_set(1,1,error,sizeof error) && !actual_fullscreen && g.ready);
    assert(g.backbuf[0] && g.backbuf[1]);
    fail_buffer_call=get_buffer_calls+2; // one newly acquired buffer before failure
    assert(!haze_video_display_set(1,1,error,sizeof error) && !actual_fullscreen && g.ready);
    assert(g.backbuf[0] && g.backbuf[1]);
    assert(haze_video_display_set(1,0,error,sizeof error) && g_ld_fullscreen_requested && !actual_fullscreen); // focus-loss retry policy
    assert(!haze_video_display_set(0,1,error,sizeof error));
    high.sharpening=0;
    assert(haze_video_prepare(&high,error,sizeof error));
    assert(!haze_video_display_set(1,1,error,sizeof error)); haze_video_finish(0);
    owner_thread=0; assert(!haze_video_display_set(1,1,error,sizeof error)); owner_thread=GetCurrentThreadId();
    removed=1; assert(!haze_video_display_set(1,1,error,sizeof error) && !actual_fullscreen); removed=0;
    all_resize_fail=1;
    assert(!haze_video_display_set(1,1,error,sizeof error) && !g.ready && !actual_fullscreen && !g_ld_fullscreen_requested);
    assert(strstr(error,"Restart Haze")!=nullptr);
    restored_display=0;
    g.queue=&queue; g.fence=&fence; g.swap=&swap_chain; g.ready=1; g_ld_fullscreen_requested=1;
    const auto retained=g_ld_postprocess.output;
    bundle_stop_cost=8;
    recipe_stop_cost=3;
    assert(haze_video_stop(123));
    assert(bundle_stop_calls==1 && bundle_stop_ready==1 && bundle_stop_limit==123);
    assert(recipe_stop_calls==2 && recipe_stop_limit==115);
    assert(wait_limit==112 && restored_display==1 && timer_destroyed==1 && !g.ready && !g_ld_fullscreen_requested);
    assert(g.queue==&queue && g.fence==&fence && g_ld_postprocess.output==retained); // No in-flight resource release.
    g.ready=1; wait_result=WAIT_TIMEOUT; wait_limit=0;
    bundle_stop_cost=25;
    assert(!haze_video_stop(17)); assert(wait_limit==0 && restored_display==2 && !g.ready);
    assert(recipe_stop_calls==4 && recipe_stop_limit==0);
    assert(bundle_stop_calls==2 && bundle_stop_ready==1 && bundle_stop_limit==17);
    bundle_stop_cost=0;
    g.ready=1; signal_failure=1; wait_limit=0;
    assert(!haze_video_stop(19)); assert(wait_limit==0 && restored_display==3);
    signal_failure=0; g.ready=1; event_failure=1;
    assert(!haze_video_stop(19)); assert(wait_limit==0 && restored_display==4);
    event_failure=0; completed_value=UINT64_MAX; g.ready=1; restore_failure=1;
    assert(!haze_video_stop(19)); assert(wait_limit==0 && restored_display==5);
    puts("Live renderer transaction: allocation/fence/device failure, cancellation, sampler invalidation and effect lifetime passed.");
}
