#include "haze_movie.h"
#include "haze_movie_decode.h"
#include "haze_pamf.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
namespace {
using namespace haze_movie;
using Clock=std::chrono::steady_clock;
std::mutex control;
std::thread worker;
std::once_flag exit_hook;
std::atomic<bool> stop=false,done=false,paused=false;
uint32_t owner=0;
std::string disc_root;
std::mutex presentation;
std::optional<VideoFrame> pending_frame;
bool requested_mode=false;
uint64_t generation=0;
void request_mode(bool on) {
    std::lock_guard lock(presentation);
    requested_mode=on; pending_frame.reset(); if(on) ++generation;
}
std::string movie_path(const char* uri) {
    constexpr const char* prefix="x-cell-fs:///dev_bdvd/";
    require(uri && !strncmp(uri,prefix,strlen(prefix)),"Unsupported movie URI");
    const char* root=disc_root.empty()?getenv("PS3_VFS_ROOT"):disc_root.c_str();
    require(root && *root,"Movie VFS root is unset");
    std::filesystem::path relative=std::filesystem::u8path(uri+strlen(prefix));
    require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name(),"Invalid movie path");
    for(const auto& part:relative) require(part!="..","Invalid movie path traversal");
    return (std::filesystem::u8path(root)/relative).string();
}
void play(std::string path) {
    SDL_AudioStream* output=nullptr;
    bool audio_initialized=false;
    bool rendering=false;
    uint64_t presented=0,audio_count=0;
    try {
        std::deque<VideoFrame> frames;
        int64_t submitted=0;
        double audio_end=0,video_end=0;
        auto video=[&](VideoFrame&& f) {
            require(frames.size()<64,"Movie video queue exceeded its bound");
            video_end=f.pts;
            frames.push_back(std::move(f));
        };
        auto audio=[&](AudioFrame&& f) {
            const int64_t position=llround(f.pts*48000);
            const int64_t count=int64_t(f.stereo.size()/2);
            const int64_t trim=std::clamp(submitted-position,int64_t(0),count);
            if(position>submitted) {
                require(position-submitted<=48000*4,"Oversized movie audio gap");
                std::vector<float> silence(size_t(position-submitted)*2,0);
                if(output) require(SDL_PutAudioStreamData(output,silence.data(),int(silence.size()*4)),SDL_GetError());
                submitted=position;
            }
            const int remaining=int(count-trim);
            if(output && remaining) require(SDL_PutAudioStreamData(output,f.stereo.data()+trim*2,remaining*8),SDL_GetError());
            submitted+=remaining; audio_count+=remaining; audio_end=submitted/48000.0;
            if(output) require(SDL_GetAudioStreamQueued(output)<=48000*8*4,"Movie audio queue exceeded its bound");
        };
        Decoder decoder(path.c_str(),video,audio);
        if(decoder.has_audio()) {
            if((audio_initialized=SDL_InitSubSystem(SDL_INIT_AUDIO))) {
                SDL_AudioSpec spec{SDL_AUDIO_F32,2,48000};
                output=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
            }
            if(!output) fprintf(stderr,"[movie] Audio device unavailable: %s; using presentation clock.\n",SDL_GetError());
            const char* mute=getenv("AUDIO_MUTE");
            if(output && mute && atoi(mute)) SDL_SetAudioStreamGain(output,0);
        }
        fprintf(stderr,"[movie] Playing %s duration=%.3f audio=%d\n",path.c_str(),decoder.duration(),decoder.has_audio());
        bool eof=false,started=false,was_paused=false;
        double time=0,clock_base=0;
        auto epoch=Clock::now();
        while(!stop) {
            if(paused) {
                if(!was_paused && output) SDL_PauseAudioStreamDevice(output);
                was_paused=true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue;
            }
            if(was_paused) {
                epoch=Clock::now(); clock_base=time;
                if(output && started) SDL_ResumeAudioStreamDevice(output);
                was_paused=false;
            }
            if(started) {
                const int queued=output?SDL_GetAudioStreamQueued(output):0;
                require(queued>=0,"Movie audio device failed");
                if(output && (!eof || queued>0)) {
                    time=std::max(time,(submitted-queued/8)/48000.0);
                    epoch=Clock::now(); clock_base=time;
                } else time=clock_base+std::chrono::duration<double>(Clock::now()-epoch).count();
            }
            // Decode a little ahead of both streams. PES interleaving and B-frame
            // reordering require more than one frame of lookahead.
            unsigned packets=0;
            while(!eof && !stop && packets++<256 &&
                  (video_end<time+.3 || (decoder.has_audio() && audio_end<time+.3))) {
                eof=!decoder.step();
            }
            if(!started && (eof || (video_end>=.3 && (!decoder.has_audio() || audio_end>=.3)))) {
                request_mode(true); rendering=true;
                epoch=Clock::now(); started=true;
                if(output) require(SDL_ResumeAudioStreamDevice(output),SDL_GetError());
            }
            if(started) {
                // Drop late frames without delaying audio or allowing drift.
                while(frames.size()>1 && frames[1].pts<=time) frames.pop_front();
                if(!frames.empty() && frames.front().pts<=time+.002) {
                    auto f=std::move(frames.front()); frames.pop_front();
                    std::lock_guard lock(presentation);
                    pending_frame=std::move(f); ++presented;
                }
                if(eof && frames.empty() && time>=std::max(decoder.duration(),audio_end)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        fprintf(stderr,"[movie] %s frames=%llu audio_samples=%llu time=%.3f\n",
                stop?"Stopped":"EOF",(unsigned long long)presented,(unsigned long long)audio_count,time);
    } catch(const std::exception& error) {
        fprintf(stderr,"[movie] Playback failed: %s (%s)\n",error.what(),path.c_str());
    }
    if(output) SDL_DestroyAudioStream(output);
    if(audio_initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if(rendering) request_mode(false);
    done=true;
}
void stop_locked() {
    stop=true;
    if(worker.joinable()) worker.join();
    owner=0; done=false;
}
}
extern "C" int haze_movie_start(uint32_t player,const char* uri) {
    std::call_once(exit_hook,[]{ std::atexit(haze_movie_shutdown); });
    std::lock_guard lock(control);
    stop_locked();
    try {
        const auto path=movie_path(uri);
        Pamf validate(path.c_str());
        owner=player; stop=false; done=false; paused=false;
        worker=std::thread(play,path); return 1;
    } catch(const std::exception& error) {
        fprintf(stderr,"[movie] Cannot start %s: %s\n",uri?uri:"",error.what()); owner=0; return 0;
    }
}
extern "C" void haze_movie_set_root(const char* root) {
    std::lock_guard lock(control); disc_root=root?root:"";
}
extern "C" void haze_movie_stop(uint32_t player) {
    std::lock_guard lock(control); if(owner==player) stop_locked();
}
extern "C" int haze_movie_finished(uint32_t player) {
    std::lock_guard lock(control); return owner==player && done;
}
extern "C" void haze_movie_pause(uint32_t player,int value) {
    std::lock_guard lock(control); if(owner==player) paused=value!=0;
}
extern "C" void haze_movie_shutdown() {
    std::lock_guard lock(control); stop_locked();
}
// DXGI presentation can synchronously message its window. Keep it on the
// window/FIFO thread; waiting for that thread from a decoder holding the RSX
// lock deadlocks the window's next FIFO drain. The mailbox holds one frame.
extern "C" int haze_movie_present_tick() {
    static bool active=false;
    static uint64_t shown_generation=0;
    std::optional<VideoFrame> frame;
    bool requested; uint64_t current;
    {
        std::lock_guard lock(presentation);
        requested=requested_mode; current=generation; frame=std::move(pending_frame); pending_frame.reset();
    }
    if(active && (!requested || current!=shown_generation)) {
        haze_movie_render_mode(0); active=false;
    }
    if(requested && !active) {
        haze_movie_render_mode(1); active=true; shown_generation=current;
    }
    if(active && frame) haze_movie_render_frame(frame->rgba.data(),frame->width,frame->height);
    return active;
}
