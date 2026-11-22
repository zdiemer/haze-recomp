#include "haze_character_lod.h"
// Patched guest 00245CE8: promote the reduced character part masks to the
// closest-range mask while Full character detail is selected.
int haze_character_lod_full() {
    return haze_live_character_full_detail.load(std::memory_order_relaxed) ? 1 : 0;
}
