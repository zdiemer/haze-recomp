#include "haze_movie_decode.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
int main(int argc,char** argv) {
    if(argc<2) return 2;
    try {
        for(int i=1;i<argc;++i) {
            uint64_t video=0,samples=0,hash=1469598103934665603ull;
            double first_video=-1,last_video=-1,first_audio=0,last_audio=0,energy=0,peak=0;
            haze_movie::Decoder decoder(argv[i],[&](haze_movie::VideoFrame&& f) {
                assert(f.pts>last_video);
                if(!video) first_video=f.pts;
                last_video=f.pts; ++video;
                assert(f.rgba.size()==size_t(f.width)*f.height*4);
                for(size_t p=0;p<f.rgba.size();p+=101) hash=(hash^f.rgba[p])*1099511628211ull;
            },[&](haze_movie::AudioFrame&& f) {
                if(!samples) first_audio=f.pts;
                else assert(std::abs(f.pts-last_audio)<.0001);
                for(float value:f.stereo) {
                    assert(std::isfinite(value)); energy+=double(value)*value; peak=std::max(peak,std::abs(double(value)));
                }
                samples+=f.stereo.size()/2; last_audio=f.pts+f.stereo.size()/2/48000.0;
            });
            while(decoder.step()) {}
            assert(!decoder.step()); // EOF must not flush twice
            assert(video>1 && first_video>=-.001 && first_video<.1);
            assert(std::abs(last_video-decoder.duration())<.1);
            if(decoder.has_audio()) {
                assert(samples>0 && energy>0 && peak>0);
                assert(std::abs(last_audio-decoder.duration())<.15);
            }
            printf("%s frames=%llu pts=%.6f..%.6f duration=%.6f hash=%016llx audio_samples=%llu pts=%.6f..%.6f rms=%.6f peak=%.6f\n",
                argv[i],(unsigned long long)video,first_video,last_video,decoder.duration(),(unsigned long long)hash,
                (unsigned long long)samples,first_audio,last_audio,samples?sqrt(energy/(samples*2)):0,peak);
        }
    } catch(const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
