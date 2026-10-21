#pragma once

// Asset-free declarations for the guarded wait boundary. Use the SDK's real
// context layout; test_job_wait.cpp supplies guest RAM and the guest call.
#include "ppu_context.h"

extern "C" {
uint8_t vm_read8(uint64_t address);
uint32_t vm_read32(uint64_t address);
uint64_t vm_read64(uint64_t address);
}
void func_0068EFC0(ppu_context*);
