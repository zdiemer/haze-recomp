#pragma once
#include <stdint.h>
#include <string.h>

// A PC operation over ordinary host data; no guest registers or LS semantics.
typedef struct haze_native_cursor {
    float value, increment;
    uint32_t index, remaining;
    float secondary, secondary_increment;
} haze_native_cursor;
#ifdef __cplusplus
static_assert(sizeof(haze_native_cursor)==24,"Cursor boundary requires six 32-bit fields");
#else
_Static_assert(sizeof(haze_native_cursor)==24,"Cursor boundary requires six 32-bit fields");
#endif
// The boundary supplies the game's scalar rounding/range policy. No execution
// context or vector lanes are needed by this operation over host data.
static inline void haze_native_cursor_advance(haze_native_cursor* cursor,
    uint32_t (*add_bits)(uint32_t, uint32_t))
{
    uint32_t words[6];
    memcpy(words,cursor,sizeof(words));
    words[0]=add_bits(words[0],words[1]);
    ++words[2];
    --words[3];
    words[4]=add_bits(words[4],words[5]);
    memcpy(cursor,words,sizeof(words));
}
