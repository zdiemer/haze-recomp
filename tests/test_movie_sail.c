#include <assert.h>
#include "../port/haze_sail_backend.c"
uint8_t* vm_base;
ps3_guest_caller_fn g_ps3_guest_caller;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t v,int w) { (void)ea;(void)v;(void)w; }
static u32 playing;
static int finished,fail,paused,starts,stops,opened,running;
int haze_movie_start(uint32_t player,const char* uri) { (void)uri; playing=fail?0:player; finished=0; return !fail; }
void haze_movie_stop(uint32_t player) { if(playing==player) playing=0; }
int haze_movie_finished(uint32_t player) { return playing==player && finished; }
void haze_movie_pause(uint32_t player,int value) { assert(playing==player); paused=value; }
void haze_movie_shutdown(void) { playing=0; }
static void host(u32 player,const char* uri,int start,void* arg) {
    (void)player;(void)uri;(void)arg; if(start) starts++; else stops++;
}
static void notify(uint32_t opd,uint64_t user,uint64_t event,uint64_t value,
                   uint64_t a3,uint64_t a4,uint64_t a5,uint64_t a6,uint64_t a7) {
    (void)opd;(void)user;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;
    if(event==((u64)3<<32)) { if(value==4) opened++; if(value==6) running++; }
}
int main(void) {
    vm_base=calloc(1,0x10000); assert(vm_base);
    g_ps3_guest_caller=notify; cellSail_set_host_playback_callback(host,0);
    assert(cellSailPlayerInitialize(0x1000,1,0x2000,0x4000,0,2,1,0,0)==0);
    assert(cellSailPlayerStart(0x1000)==0);
    assert(playing==0x1000 && running==1 && opened==0 && starts==1);
    assert(!haze_sail_movie_ended(0x1000));
    assert(cellSailPlayerPause(0x1000)==0 && paused);
    assert(cellSailPlayerStart(0x1000)==0 && !paused && starts==1);
    finished=1;
    assert(haze_sail_movie_ended(0x1000));
    assert(!playing && opened==1 && stops==1);
    assert(haze_sail_movie_ended(0x1000) && opened==1 && stops==1);
    assert(cellSailPlayerStart(0x1000)==0 && !haze_sail_movie_ended(0x1000));
    assert(cellSailPlayerStop(0x1000)==0 && !playing);
    assert(cellSailPlayerStart(0x1000)==0);
    assert(cellSailPlayerCloseStream(0x1000)==0 && !playing);
    assert(cellSailPlayerStart(0x1000)==0);
    assert(cellSailPlayerCancel(0x1000)==0 && !playing);
    fail=1; assert(cellSailPlayerStart(0x1000)==0 && haze_sail_movie_ended(0x1000));
    fail=0; assert(cellSailPlayerStart(0x1000)==0);
    assert(cellSailPlayerFinalize(0x1000)==0 && !playing);
    assert(!haze_sail_movie_ended(0x1000));
    assert(cellSailPlayerStart(0x1000)<0);
    free(vm_base); puts("Movie SAIL running, pause/resume, EOF, skip, close, cancel, failure and finalize passed.");
}
