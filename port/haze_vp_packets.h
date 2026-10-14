#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Host-only packing kernel for complete 128-byte vertex-program blocks.
// Input is already in the command stream's byte order. The caller owns both
// spans exclusively until publication; this routine performs no guest memory
// access, reservation handling, command submission, or pointer publication.
// Runtime integration must establish those contracts before calling it.
namespace haze {
inline bool pack_vp_blocks(const uint8_t* source, size_t source_size,
                           uint8_t* destination, size_t destination_size,
                           size_t blocks) {
    if (!blocks) return true;
    if (!source || !destination || blocks > source_size / 128 ||
        blocks > destination_size / 132) return false;
    const size_t input_bytes = blocks * 128, output_bytes = blocks * 132;
    const uintptr_t src = reinterpret_cast<uintptr_t>(source);
    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination);
    constexpr auto max_address = std::numeric_limits<uintptr_t>::max();
    if (src > max_address - input_bytes || dst > max_address - output_bytes)
        return false;
    if (src < dst + output_bytes && dst < src + input_bytes) return false;
    for (size_t i = 0; i < blocks; ++i) {
        // 32 words at RSX method 0xB80: big-endian command 0x00800B80.
        destination[0] = 0; destination[1] = 0x80;
        destination[2] = 0x0b; destination[3] = 0x80;
        std::memcpy(destination + 4, source, 128);
        source += 128; destination += 132;
    }
    return true;
}
}
