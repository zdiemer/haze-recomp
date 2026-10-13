/* Keep the pinned audio implementation in one TU. Native process shutdown
 * stops output without destroying port state that guest workers may still
 * touch before the OS ends their threads. */
#include "cellAudio.c"

int haze_audio_stop_output(unsigned timeout_ms) {
    s_mix_thread_running=0;
    if (s_mix_thread) {
        const DWORD result=WaitForSingleObject(s_mix_thread,timeout_ms);
        if (result!=WAIT_OBJECT_0) {
            fprintf(stderr,"[quit] Audio worker did not stop (wait=%lu); retaining its resources until process exit.\n",result);
            return 0;
        }
        CloseHandle(s_mix_thread); s_mix_thread=NULL;
    }
    audio_backend_shutdown();
    /* Deliberately keep s_audio_initialized, ports and mutex intact. The
     * parked main producer cannot reinitialize audio, and outstanding guest
     * worker calls must not encounter a destroyed lock or freed buffers. */
    fprintf(stderr,"[quit] Audio output stopped.\n");
    return 1;
}
