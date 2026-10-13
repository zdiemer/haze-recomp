#pragma once
#include <cstdint>

// Observation of Haze's serialized native save worker. Starting a thread is
// not a successful save. Result becomes available after the guest has handled
// the save-data return code, immediately before its worker exits.
struct HazeSaveStatus {
    uint64_t ticket=0;
    bool busy=false,completed=false;
    int32_t result=0;
    bool settings=false;
};
HazeSaveStatus haze_save_status();
HazeSaveStatus haze_save_status(uint64_t ticket);
uint64_t haze_save_begin(uint32_t worker);
// The launcher must return failure without starting a worker for this ticket.
constexpr uint64_t HAZE_SAVE_REJECTED=UINT64_MAX;
// Atomically close admission only after the current save finishes. The caller
// must also quiesce the guest/settings producers before committing shutdown.
bool haze_save_seal(uint64_t expected_ticket);
void haze_save_unseal();
uint64_t haze_save_current();
void haze_save_launched(uint64_t ticket,int32_t result);
void haze_save_finished(uint64_t ticket,int32_t result);
// Arm only around the native Audio Apply call on its owning guest thread.
void haze_save_settings_arm(bool armed);
extern "C" int haze_save_is_settings(void);
