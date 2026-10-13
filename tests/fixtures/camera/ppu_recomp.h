#pragma once
#include <cstdint>
struct ppu_context { uint64_t gpr[32]; double fpr[32]; };
uint8_t vm_read8(uint64_t);
uint32_t vm_read32(uint64_t);
void vm_write32(uint64_t,uint32_t);
void vm_write8(uint64_t,uint8_t);
void func_0024D3A0(ppu_context*);
void func_00095EA8(ppu_context*);
void func_002EFC20(ppu_context*);
void func_002F2978(ppu_context*);
void func_00311060(ppu_context*);
void func_00098038(ppu_context*);
void func_00305510(ppu_context*);
