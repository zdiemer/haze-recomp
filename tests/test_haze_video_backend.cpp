#include <cassert>
#include "../port/haze_video_backend.c"
int main() {
    g.dev=&device; g.ready=1; owner_thread=GetCurrentThreadId();
    g.n_samplers=3; g.smp_ring_used=2; g.smp_keys[0]=42;
    g_ld_anisotropy=1;
    haze_video_live high={1,16,60,0,.15f,0,-1}; char error[200];
    allocation_failure=1;
    assert(!haze_video_prepare(&high,error,sizeof error));
    assert(fences==0 && !g_ld_postprocess.output && g_ld_anisotropy==1 && g.n_samplers==3);
    allocation_failure=0;
    assert(haze_video_prepare(&high,error,sizeof error));
    assert(fences==1 && allocated==1 && !g_ld_postprocess.output && g_ld_anisotropy==1);
    assert(!haze_video_prepare(&high,error,sizeof error)); // No overlapping transactions.
    haze_video_finish(0);
    assert(released==1 && g.n_samplers==3 && g_ld_anisotropy==1);
    assert(haze_video_prepare(&high,error,sizeof error)); haze_video_finish(1);
    assert(g_ld_postprocess.output && g_ld_anisotropy==16 && !g.n_samplers && !g.smp_ring_used && !g.smp_keys[0]);
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
    puts("Live renderer transaction: allocation/fence/device failure, cancellation, sampler invalidation and effect lifetime passed.");
}
