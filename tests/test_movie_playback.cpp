#include "haze_movie.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
static std::atomic<unsigned> frames=0;
static std::atomic<bool> mode=false;
static void pump(int ms) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    do { haze_movie_present_tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    while(std::chrono::steady_clock::now()<until);
}
extern "C" void haze_movie_render_mode(int on) { mode=on!=0; }
extern "C" void haze_movie_render_frame(const uint8_t* data,uint32_t w,uint32_t h) {
    assert(mode && data && w==1280 && h==720); ++frames;
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    haze_movie_set_root(argv[1]);
    _putenv_s("PS3_VFS_ROOT",""); _putenv_s("AUDIO_MUTE","1");
    constexpr const char* uri="x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/assets/g5/pamf/frd.pam";
    using namespace std::chrono;
    assert(!haze_movie_start(1,"x-cell-fs:///dev_bdvd/../missing.pam"));
    assert(haze_movie_start(1,uri));
    const auto start=steady_clock::now();
    while(!haze_movie_finished(1) && steady_clock::now()-start<seconds(10)) pump(10);
    haze_movie_present_tick();
    assert(haze_movie_finished(1) && !mode && frames>140);
    const double elapsed=duration<double>(steady_clock::now()-start).count();
    assert(elapsed>5.8 && elapsed<7.5);
    haze_movie_stop(1);
    frames=0; assert(haze_movie_start(2,uri));
    pump(700);
    haze_movie_pause(2,1); pump(80);
    unsigned before=frames;
    pump(200); assert(frames==before);
    haze_movie_pause(2,0); pump(200); assert(frames>before);
    haze_movie_stop(99); assert(mode);
    auto stop=steady_clock::now(); haze_movie_stop(2);
    haze_movie_present_tick();
    assert(!mode && steady_clock::now()-stop<seconds(1));
    assert(haze_movie_start(3,uri));
    pump(200);
    assert(haze_movie_start(4,uri));
    haze_movie_shutdown(); haze_movie_present_tick(); assert(!mode);
    printf("Movie playback EOF %.3fs, pause/resume, skip, owner isolation, replacement and shutdown passed.\n",elapsed);
}
