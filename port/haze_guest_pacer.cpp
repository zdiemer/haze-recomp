#include "ppu_recomp.h"
#include "haze_frame_pacer_settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

// Selecting an unlimited or >30 PC cap opts into the native pacing path.
// The renderer limiter and VSync own presentation timing when this is enabled.
int haze_guest_pacer_bypass()
{
#ifdef _WIN32
    static const bool supported = vm_read32(0xE94EE0-0x7D70)==0xE35AE0 &&
        vm_read32(0xE35AE0-0x7FFC)==0x169F484 &&
        vm_read32(0xE35AE0-0x7F90)==0x1028B90;
    static const char* control=std::getenv("HAZE_FRAME_PACER_FILE");
    static int enabled=[] {
        return haze_native_pacer_selected(std::getenv("HAZE_NATIVE_FRAME_PACER"),
            std::getenv("RSX_MAX_FPS")) ? 1 : 0;
    }();
    static ULONGLONG next_check=0;
    static int reported=-1;
    static const char* explicit_mode=std::getenv("HAZE_NATIVE_FRAME_PACER");
    const int live_limit=haze_live_frame_limit.load();
    if (!control && live_limit>=0 && (!explicit_mode || !*explicit_mode))
        enabled=live_limit==0 || live_limit>30;
    if (control && GetTickCount64()>=next_check) {
        next_check=GetTickCount64()+100;
        FILE* file=std::fopen(control,"r"); unsigned value;
        if (file) {
            if (std::fscanf(file,"%u",&value)==1 && value<=1) enabled=(int)value;
            std::fclose(file);
        }
    }
    const int effective=supported && enabled;
    if (reported!=effective) {
        if (control || enabled) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            std::fprintf(stderr,"[haze-frame-pacer-mode] tid=%lu enabled=%d qpc=%lld\n",
                GetCurrentThreadId(),effective,now.QuadPart);
            if (!supported)
                std::fprintf(stderr,"[haze-frame-pacer] Unsupported Haze globals; retaining guest pacing\n");
        }
        reported=effective;
    }
    return effective;
#else
    return 0;
#endif
}
