/* Keep the pinned guest ABI/descriptor implementation, replacing only playback.
 * Decoder threads never call guest code: completion is delivered by a guest
 * frame/state query after decoding/audio drain. Renderer ownership changes
 * at the next window-thread tick, before any further guest presentation. */
#define cellSailPlayerStart stub_sail_start
#define cellSailPlayerStop stub_sail_stop
#define cellSailPlayerCloseStream stub_sail_close
#define cellSailPlayerCancel stub_sail_cancel
#define cellSailPlayerDestroy stub_sail_destroy
#define cellSailPlayerFinalize stub_sail_finalize
#define cellSailPlayerPause stub_sail_pause
#define cellSailPlayerGetState stub_sail_state
#define cellSailGraphicsAdapterGetFrame2 stub_sail_frame
#define cellSailTerm stub_sail_term
#include "cellSail.c"
#undef cellSailPlayerStart
#undef cellSailPlayerStop
#undef cellSailPlayerCloseStream
#undef cellSailPlayerCancel
#undef cellSailPlayerDestroy
#undef cellSailPlayerFinalize
#undef cellSailPlayerPause
#undef cellSailPlayerGetState
#undef cellSailGraphicsAdapterGetFrame2
#undef cellSailTerm
#include "haze_movie.h"
#include <windows.h>
static volatile LONG movie_player;
static void movie_stop(u32 player) {
    InterlockedCompareExchange(&movie_player,0,(LONG)player);
    haze_movie_stop(player);
}
static void movie_poll(void) {
    u32 player=(u32)InterlockedCompareExchange(&movie_player,0,0);
    if(!player || !haze_movie_finished(player) ||
       (u32)InterlockedCompareExchange(&movie_player,0,(LONG)player)!=player) return;
    haze_movie_stop(player);
    const int slot=sail_player_slot(player);
    if(slot<0) return;
    s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_host_playback(slot,0);
    sail_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_STOP_TRANSITION);
    sail_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_OPENED);
}
int haze_sail_movie_ended(u32 player) {
    movie_poll();
    const int slot=sail_player_slot(player);
    return slot>=0 && s_sail_players[slot].state==CELL_SAIL_PLAYER_STATE_FINISHED;
}
s32 cellSailPlayerStart(CellSailPlayerHandle player) {
    const int slot=sail_player_slot(player);
    if(slot<0) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if(s_sail_players[slot].state==CELL_SAIL_PLAYER_STATE_PAUSE) {
        haze_movie_pause(player,0);
        s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_RUNNING;
        sail_call(slot,SAIL_CALL_START,SAIL_ST_START_TRANSITION,SAIL_ST_RUNNING);
        return CELL_OK;
    }
    const u32 previous=(u32)InterlockedExchange(&movie_player,0);
    if(previous) {
        haze_movie_stop(previous);
        const int old=sail_player_slot(previous);
        if(old>=0) { s_sail_players[old].state=CELL_SAIL_PLAYER_STATE_FINISHED; sail_host_playback(old,0); }
    }
    const int started=haze_movie_start(player,s_sail_players[slot].uri);
    s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_RUNNING;
    sail_call(slot,SAIL_CALL_START,SAIL_ST_START_TRANSITION,SAIL_ST_RUNNING);
    sail_host_playback(slot,1);
    if(started) InterlockedExchange(&movie_player,(LONG)player);
    else {
        // A missing/unsupported stream must release the guest's movie wait.
        s_sail_players[slot].state=CELL_SAIL_PLAYER_STATE_FINISHED;
        sail_host_playback(slot,0);
        sail_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_STOP_TRANSITION);
        sail_notify(slot,SAIL_EV_STATE_CHANGED,0,SAIL_ST_OPENED);
    }
    return CELL_OK;
}
s32 cellSailPlayerStop(CellSailPlayerHandle player) { movie_stop(player); return stub_sail_stop(player); }
s32 cellSailPlayerCloseStream(CellSailPlayerHandle player) { movie_stop(player); return stub_sail_close(player); }
s32 cellSailPlayerCancel(CellSailPlayerHandle player) { movie_stop(player); return stub_sail_cancel(player); }
s32 cellSailPlayerDestroy(CellSailPlayerHandle player) { movie_stop(player); return stub_sail_destroy(player); }
s32 cellSailPlayerFinalize(u32 player) { movie_stop(player); return stub_sail_finalize(player); }
s32 cellSailPlayerPause(CellSailPlayerHandle player) {
    s32 result=stub_sail_pause(player); if(result==CELL_OK) haze_movie_pause(player,1); return result;
}
s32 cellSailPlayerGetState(CellSailPlayerHandle player,s32* state) { movie_poll(); return stub_sail_state(player,state); }
s32 cellSailGraphicsAdapterGetFrame2(u32 self,u32 info,u32 prev,u32 flip,u64 flags) {
    movie_poll(); return stub_sail_frame(self,info,prev,flip,flags);
}
s32 cellSailTerm(void) { InterlockedExchange(&movie_player,0); haze_movie_shutdown(); return stub_sail_term(); }
