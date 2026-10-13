#include <cassert>
#include <cstdio>
#include <thread>
#include "../port/haze_save_status.cpp"
int main() {
    assert(!haze_save_begin(0) && !haze_save_status().busy);
    auto ticket=haze_save_begin(0xe71ac8);
    assert(ticket && haze_save_current()==ticket && haze_save_status().busy);
    haze_save_launched(ticket,0);
    assert(haze_save_status().busy && !haze_save_status().completed);
    haze_save_finished(ticket,0);
    assert(!haze_save_status().busy && haze_save_status().completed && !haze_save_status().result);
    auto next=haze_save_begin(0xe71ac8);
    haze_save_finished(ticket,-1); // A stale completion cannot finish a new request.
    assert(haze_save_status().busy && !haze_save_status().result);
    haze_save_launched(next,-5);
    assert(haze_save_status().completed && haze_save_status().result==-5);
    next=haze_save_begin(0xe71ac8);
    haze_save_finished(next,(int32_t)0x8002b401);
    haze_save_launched(next,0); // Fast worker completion precedes launch return.
    assert(haze_save_status().completed && haze_save_status().result==(int32_t)0x8002b401);
    haze_save_finished(next,0); // Completion is terminal.
    assert(haze_save_status().result==(int32_t)0x8002b401 && !haze_save_current());
    haze_save_settings_arm(true);
    std::thread unrelated([] {
        auto ticket=haze_save_begin(0xe71ac8); assert(!haze_save_is_settings()); haze_save_finished(ticket,0);
    }); unrelated.join();
    next=haze_save_begin(0xe71ac8); assert(haze_save_is_settings() && haze_save_status().settings);
    haze_save_finished(next,0); assert(!haze_save_is_settings());
    next=haze_save_begin(0xe71ac8); assert(!haze_save_is_settings()); haze_save_finished(next,0);
    haze_save_settings_arm(true); haze_save_settings_arm(false);
    next=haze_save_begin(0xe71ac8); assert(!haze_save_is_settings()); haze_save_finished(next,0);
    std::puts("Native save status: worker completion, launch failure and stale results passed.");
}
