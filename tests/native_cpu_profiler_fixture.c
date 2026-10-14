#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile uint64_t result;
static volatile LONG g_frames_presented;
__declspec(noinline) uint64_t oracle_stall(uint64_t value)
{
    Sleep(250);
    result = value;
    return result;
}
__declspec(noinline) uint64_t oracle_leaf(uint64_t value)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        result = value;
    }
    return value;
}
__declspec(noinline) uint64_t oracle_middle(uint64_t value)
{
    return oracle_leaf(value) + 3;
}
__declspec(noinline) uint64_t oracle_entry(uint64_t value)
{
    return oracle_middle(value) + 7;
}
int main(int argc, char** argv)
{
    unsigned seconds = argc >= 2 ? (unsigned)atoi(argv[1]) : 8;
    if (!seconds || seconds > 20) return 2;
    printf("%lu\n", GetCurrentThreadId());
    fflush(stdout);
    ULONGLONG start = GetTickCount64(), deadline = start + 1000 * seconds;
    ULONGLONG next_stall = start + 1000;
    uint64_t value = 1;
    while (GetTickCount64() < deadline) {
        value = oracle_entry(value);
        InterlockedIncrement(&g_frames_presented);
        if (argc >= 3 && !strcmp(argv[2], "stall") && GetTickCount64() >= next_stall) {
            LARGE_INTEGER begin, end;
            QueryPerformanceCounter(&begin);
            value = oracle_stall(value);
            QueryPerformanceCounter(&end);
            printf("STALL %lld %lld\n", begin.QuadPart, end.QuadPart);
            fflush(stdout);
            next_stall = GetTickCount64() + 750;
        }
    }
    return 0;
}
