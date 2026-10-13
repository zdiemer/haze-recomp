#include "haze_save_status.h"
#include <mutex>
#include <cstdio>
#include <deque>

namespace {
std::mutex mutex;
HazeSaveStatus status;
std::deque<HazeSaveStatus> history;
bool sealed=false;
thread_local bool settings_armed=false;
}
void haze_save_settings_arm(bool armed) { settings_armed=armed; }
extern "C" int haze_save_is_settings(void) {
    std::lock_guard lock(mutex); return status.busy && status.settings;
}
HazeSaveStatus haze_save_status() {
    std::lock_guard lock(mutex); return status;
}
HazeSaveStatus haze_save_status(uint64_t ticket) {
    std::lock_guard lock(mutex);
    if (ticket && status.ticket==ticket) return status;
    for (const auto& previous:history) if (previous.ticket==ticket) return previous;
    return {};
}
uint64_t haze_save_begin(uint32_t worker) {
    // The common launcher also starts load workers. Only observe the audited
    // save worker descriptor; do not reinterpret load completion as a save.
    if (worker!=0xe71ac8) return 0;
    std::lock_guard lock(mutex);
    if (sealed || status.busy) return HAZE_SAVE_REJECTED;
    if (status.ticket) { history.push_back(status); if (history.size()>16) history.pop_front(); }
    ++status.ticket; status.busy=true; status.completed=false; status.result=0;
    status.settings=settings_armed; settings_armed=false;
    std::fprintf(stderr,"[settings-save] Started save %llu\n",(unsigned long long)status.ticket);
    return status.ticket;
}
bool haze_save_seal(uint64_t expected_ticket) {
    std::lock_guard lock(mutex);
    if (status.busy || status.ticket!=expected_ticket) return false;
    sealed=true; return true;
}
void haze_save_unseal() {
    std::lock_guard lock(mutex); sealed=false;
}
uint64_t haze_save_current() {
    std::lock_guard lock(mutex); return status.busy ? status.ticket : 0;
}
void haze_save_finished(uint64_t ticket,int32_t result) {
    std::lock_guard lock(mutex);
    if (!ticket || ticket!=status.ticket || !status.busy) return;
    status.busy=false; status.completed=true; status.result=result;
    std::fprintf(stderr,"[settings-save] Save %llu finished: 0x%08x\n",(unsigned long long)ticket,(unsigned)result);
}
void haze_save_launched(uint64_t ticket,int32_t result) {
    // Success means the worker owns completion. It may already have finished
    // by the time thread creation returns. Failure is terminal immediately.
    if (result) haze_save_finished(ticket,result);
}
