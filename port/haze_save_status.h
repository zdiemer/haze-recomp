#pragma once
#include <cstdint>

// Observation of Haze's serialized native save worker. Starting a thread is
// not a successful save. Result becomes available after the guest has handled
// the save-data return code, immediately before its worker exits.
struct HazeSaveStatus {
    uint64_t ticket=0;
    bool busy=false,completed=false;
    int32_t result=0;
};
HazeSaveStatus haze_save_status();
uint64_t haze_save_begin(uint32_t worker);
uint64_t haze_save_current();
void haze_save_launched(uint64_t ticket,int32_t result);
void haze_save_finished(uint64_t ticket,int32_t result);
