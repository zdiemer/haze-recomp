#include <cassert>
#include <barrier>
#include <thread>
#include <cstdio>
#include "../port/haze_save_status.cpp"
#include "../port/haze_quit.cpp"
static bool guest_ready=true;
bool haze_quit_guest_ready() { return guest_ready; }
void haze_quit_resume_guest() {}

int main() {
    guest_ready=false; assert(!haze_quit_window_close()); guest_ready=true;
    assert(haze_quit_window_close()); auto close_generation=haze_quit_state().generation;
    assert(haze_quit_poll(0,false,false)); assert(haze_quit_cancel(close_generation));
    assert(!haze_quit_request(true,0));
    assert(haze_quit_request(true,1));
    auto generation=haze_quit_state().generation;
    assert(haze_quit_state().message.find("last completed checkpoint")!=std::string::npos);
    assert(!haze_quit_request(false,1) && !haze_quit_confirm(generation-1));
    assert(haze_quit_cancel(generation) && !haze_quit_take_shutdown(true));

    assert(haze_quit_request(false,2)); generation=haze_quit_state().generation;
    auto save=haze_save_begin(0xe71ac8);
    assert(haze_quit_confirm(generation));
    assert(!haze_quit_poll(2,false,false));
    haze_save_launched(save,0); assert(!haze_quit_poll(2,false,false));
    assert(haze_quit_cancel(generation)); // Cancellation never cancels a save.
    assert(haze_save_status().busy); haze_save_finished(save,0);

    assert(haze_quit_request(true,3)); generation=haze_quit_state().generation;
    assert(haze_quit_confirm(generation));
    assert(!haze_quit_poll(3,true,false) && !haze_quit_poll(3,false,true));
    // A new, fast save after confirmation must still report failure.
    save=haze_save_begin(0xe71ac8); haze_save_finished(save,-5);
    assert(!haze_quit_poll(3,false,false));
    assert(haze_quit_state().phase==HazeQuitPhase::Failed);
    assert(!haze_quit_take_shutdown(true)); assert(haze_quit_cancel(generation));

    assert(haze_quit_request(false,4)); generation=haze_quit_state().generation;
    assert(haze_quit_confirm(generation));
    assert(!haze_quit_poll(5,false,false)); // Page changed behind delayed input.
    assert(haze_quit_state().phase==HazeQuitPhase::Failed);
    assert(haze_quit_cancel(generation));

    assert(haze_quit_request(false,5)); generation=haze_quit_state().generation;
    assert(haze_quit_confirm(generation));
    save=haze_save_begin(0xe71ac8); assert(!haze_quit_poll(5,false,false));
    haze_save_finished(save,0); assert(haze_quit_poll(5,false,false));
    assert(haze_save_begin(0xe71ac8)==HAZE_SAVE_REJECTED);
    assert(!haze_quit_take_shutdown(false)); // Producer must acknowledge parking.
    assert(haze_quit_cancel(generation));
    save=haze_save_begin(0xe71ac8); assert(save!=HAZE_SAVE_REJECTED); haze_save_finished(save,0);

    // The admission seal and a concurrent worker launch cannot both succeed.
    for (int i=0;i<40;++i) {
        const auto previous=haze_save_status().ticket;
        std::barrier start(3); bool sealed_result=false; uint64_t launched=0;
        std::thread sealing([&] { start.arrive_and_wait(); sealed_result=haze_save_seal(previous); });
        std::thread launching([&] { start.arrive_and_wait(); launched=haze_save_begin(0xe71ac8); });
        start.arrive_and_wait(); sealing.join(); launching.join();
        assert(sealed_result==(launched==HAZE_SAVE_REJECTED));
        if (launched!=HAZE_SAVE_REJECTED) {
            haze_save_finished(launched,0);
            assert(!haze_save_seal(previous)); // A fast intervening save changed the snapshot.
        }
        haze_save_unseal();
    }
    assert(haze_quit_request(false,6)); generation=haze_quit_state().generation;
    assert(haze_quit_confirm(generation)); assert(haze_quit_poll(6,false,false));
    assert(haze_quit_take_shutdown(true)); assert(!haze_quit_take_shutdown(true));
    assert(!haze_quit_cancel(generation));
    assert(haze_save_begin(0xe71ac8)==HAZE_SAVE_REJECTED && !haze_save_begin(0xe71ad8));
    std::puts("Quit/save coordination: cancellation, save completion/failure, concurrent admission, stale menu and single shutdown passed.");
}
