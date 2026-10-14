#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void report_mode(int enabled)
{
#ifdef _WIN32
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    std::fprintf(stderr,"[haze-job0-kernel-mode] enabled=%d qpc=%lld\n",enabled,now.QuadPart);
#else
    (void)enabled;
#endif
}
struct settings {
    const char* control=std::getenv("HAZE_JOB0_NATIVE_FILE");
    std::atomic<int> enabled{1};
#ifdef _WIN32
    SRWLOCK lock=SRWLOCK_INIT;
    std::atomic<ULONGLONG> next_check{0};
#endif
    settings() {
        const char* value=std::getenv("HAZE_JOB0_NATIVE");
        enabled.store(!value || std::strcmp(value,"0")!=0);
        const char* validate=std::getenv("SPU_FLOAT_NATIVE_VALIDATE");
        // The original arithmetic validator must observe every operation.
        if(validate && *validate=='1') { enabled.store(0);control=nullptr; }
        report_mode(enabled.load());
    }
};
}

// Automatic experimental path; explicit zero/control files exist for A/B runs.
extern "C" int haze_job0_kernel_enabled(void)
{
    static settings mode;
#ifdef _WIN32
    if(mode.control && GetTickCount64()>=mode.next_check.load(std::memory_order_relaxed) &&
       TryAcquireSRWLockExclusive(&mode.lock)) {
        const ULONGLONG now=GetTickCount64();
        if(now>=mode.next_check.load(std::memory_order_relaxed)) {
            mode.next_check.store(now+100,std::memory_order_relaxed);
            FILE* file=std::fopen(mode.control,"r");unsigned value;
            if(file) {
                if(std::fscanf(file,"%u",&value)==1 && value<=1 &&
                   mode.enabled.exchange((int)value)!=(int)value) report_mode((int)value);
                std::fclose(file);
            }
        }
        ReleaseSRWLockExclusive(&mode.lock);
    }
#endif
    return mode.enabled.load();
}

extern "C" void haze_job0_kernel_note(unsigned pc)
{
    static std::atomic<unsigned> reported{0};
    const unsigned bit=pc==0x3408?1u:2u;
    if(!(reported.load(std::memory_order_relaxed)&bit) &&
       !(reported.fetch_or(bit,std::memory_order_relaxed)&bit))
        std::fprintf(stderr,"[haze-job0-kernel] accepted image=30 pc=%08X\n",pc);
}
