#include "../port/haze_overlay.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
static haze_overlay_sample read_sample(uint64_t id,void* context) {
    auto& samples=*static_cast<std::vector<haze_overlay_sample>*>(context);
    return samples[(id-1)%samples.size()];
}
int main() {
    std::vector<haze_overlay_sample> samples(512);
    constexpr int64_t frequency=720000;
    assert(!haze_overlay_measure(0,512,frequency,read_sample,&samples).ready);
    for (unsigned fps:{15,30,60,120,240}) {
        for (unsigned i=1;i<=2048;++i) samples[(i-1)%512]={i,int64_t(i)*frequency/fps,i/2};
        const auto s=haze_overlay_measure(2048,512,frequency,read_sample,&samples);
        assert(s.ready && std::abs(s.fps-fps)<1e-9 && std::abs(s.interval_ms-1000.0/fps)<1e-9);
        assert(std::abs(s.guest_flips-fps/2.0)<=1.0); // distinct counter, not FPS relabeled
    }
    // Recent load change and a two-second stall use precisely the same window
    // termination as the renderer, not the startup/menu lifetime average.
    for (unsigned i=1;i<=90;++i) samples[i-1]={i,int64_t(i)*frequency/30,i};
    samples[90]={91,5*frequency,90};
    auto stalled=haze_overlay_measure(91,512,frequency,read_sample,&samples);
    assert(stalled.ready && stalled.fps==.5 && stalled.interval_ms==2000 && stalled.guest_flips==0);
    samples[90].id=0;
    assert(!haze_overlay_measure(91,512,frequency,read_sample,&samples).ready);
    for (const auto size:{std::pair{640u,480u},{960u,540u},{1280u,720u},{1920u,1080u},{2560u,1080u},{3840u,2160u},{720u,1280u}})
        for (unsigned mode:{1,2}) for (unsigned corner=0;corner<4;++corner) {
            const auto r=haze_overlay_layout(size.first,size.second,size.second,mode,corner,6);
            assert(r.x>=0 && r.y>=0 && r.x+r.width<=size.first && r.y+r.height<=size.second);
            assert(r.pixel>=1);
            if (mode==1) assert(r.width<60 && r.height<=15); // tiny single-line FPS counter
        }
    puts("Overlay: capped rates, changing load, guest flips, bounded history and corner layout passed.");
}
