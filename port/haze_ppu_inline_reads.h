#pragma once

// Forced into generated PPU translation units only. Parse the original ABI
// before remapping calls, so special reads still reach the runtime accessors.
#include "ppu_recomp.h"
#include "ppu_inline_reads.h"

#define vm_read8 ppu_inline_vm_read8
#define vm_read16 ppu_inline_vm_read16
#define vm_read32 ppu_inline_vm_read32
#define vm_read64 ppu_inline_vm_read64
