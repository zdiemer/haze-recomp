#include "haze_save_status.h"
#include <mutex>
#include <cstdio>

namespace {
std::mutex mutex;
HazeSaveStatus status;
}
HazeSaveStatus haze_save_status() {
    std::lock_guard lock(mutex); return status;
}
uint64_t haze_save_begin(uint32_t worker) {
    // The common launcher also starts load workers. Only observe the audited
    // save worker descriptor; do not reinterpret load completion as a save.
    if (worker!=0xe71ac8) return 0;
    std::lock_guard lock(mutex);
    ++status.ticket; status.busy=true; status.completed=false; status.result=0;
    std::fprintf(stderr,"[settings-save] Started save %llu\n",(unsigned long long)status.ticket);
    return status.ticket;
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
