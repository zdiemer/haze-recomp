#include <cassert>
#include "../port/haze_audio_backend.c"
int main() {
    result=WAIT_TIMEOUT;
    assert(!haze_audio_stop_output(27));
    assert(timeout==27 && !s_mix_thread_running && !closed && !stopped && s_mix_thread && s_audio_initialized);
    result=WAIT_FAILED;
    assert(!haze_audio_stop_output(31)); assert(timeout==31 && !closed && !stopped && s_mix_thread);
    result=WAIT_OBJECT_0;
    assert(haze_audio_stop_output(35));
    assert(timeout==35 && closed==1 && stopped==1 && !s_mix_thread && s_audio_initialized);
    assert(haze_audio_stop_output(35)); assert(closed==1);
    puts("Host audio shutdown: bounded waits, no premature release, and guest port/lock lifetime preserved.");
}
