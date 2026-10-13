#pragma once
#include <cstdint>
struct ppu_context { uint64_t gpr[32]; };
uint8_t vm_read8(uint64_t);
uint16_t vm_read16(uint64_t);
uint32_t vm_read32(uint64_t);
void vm_write8(uint64_t,uint8_t);
void vm_write32(uint64_t,uint32_t);
void func_0024D3A0(ppu_context*);
