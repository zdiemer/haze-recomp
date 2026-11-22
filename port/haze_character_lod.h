#pragma once
#include <atomic>
#include <cstring>
// Video setting character_detail: "full" keeps every character on its
// closest-range (highest-detail) part mask; "original" keeps the game's
// distance-based selection. Read by the patched guest selection 00245CE8
// (see scripts/patches/patch_character_lod.py).
inline std::atomic<bool> haze_live_character_full_detail{true};
inline bool haze_character_detail_full(const char* value) {
    return !value || std::strcmp(value,"original")!=0;
}
