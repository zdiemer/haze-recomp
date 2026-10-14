#include "haze_job_profile.h"
#if defined(HAZE_JOB_PROFILE) && defined(_WIN32)
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {
struct Row {
    uint64_t begin,end,job,caller,ticks[4],calls[4];
    unsigned depth;
};
constexpr unsigned capacity=65536;
const char* path() { static const char* value=std::getenv("HAZE_JOB_PROFILE_CSV"); return value; }
std::mutex row_mutex;
std::vector<Row> rows;
uint64_t recorded=0;
thread_local unsigned nesting=0;
uint64_t clock_now() { LARGE_INTEGER value; QueryPerformanceCounter(&value); return value.QuadPart; }
}
HazeJobProfileScope::HazeJobProfileScope(unsigned thread,uint64_t toc,uint64_t handle,uint64_t lr) {
    if(thread!=1 || toc!=0xE94EE0 || handle>=512 || !path()) return;
    // Allocate before the first interval, never while measuring a later loop.
    { std::lock_guard<std::mutex> lock(row_mutex); if(rows.empty()) rows.resize(capacity); }
    active=true; job=handle; caller=lr; depth=nesting++; begin=clock_now();
}
void HazeJobProfileScope::start(unsigned phase) {
    if(active) { ++calls[phase]; phase_begin=clock_now(); }
}
void HazeJobProfileScope::stop(unsigned phase) {
    if(active) ticks[phase]+=clock_now()-phase_begin;
}
HazeJobProfileScope::~HazeJobProfileScope() {
    if(!active) return;
    const uint64_t end=clock_now(); --nesting;
    Row row{begin,end,job,caller,{ticks[0],ticks[1],ticks[2],ticks[3]},
        {calls[0],calls[1],calls[2],calls[3]},depth};
    std::lock_guard<std::mutex> lock(row_mutex);
    rows[recorded++%capacity]=row;
}
void haze_job_profile_dump() {
    if(!path()) return;
    // The guest may still be finishing a loop when the window closes.
    std::vector<Row> snapshot; uint64_t total;
    { std::lock_guard<std::mutex> lock(row_mutex);
      total=recorded;
      const uint64_t first=total>capacity?total-capacity:0;
      snapshot.reserve(total-first);
      for(uint64_t i=first;i<total;++i) snapshot.push_back(rows[i%capacity]); }
    FILE* file=std::fopen(path(),"wb");
    if(!file) { std::fprintf(stderr,"[job-profile] cannot open output\n"); return; }
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    std::fprintf(file,"qpc_frequency,begin_qpc,end_qpc,job,caller,depth,predicate_ticks,delay_ticks,pending_ticks,service_ticks,predicate_calls,delay_calls,pending_calls,service_calls\n");
    for(const auto& r:snapshot)
        std::fprintf(file,"%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (unsigned long long)frequency.QuadPart,r.begin,r.end,r.job,r.caller,r.depth,
            r.ticks[0],r.ticks[1],r.ticks[2],r.ticks[3],r.calls[0],r.calls[1],r.calls[2],r.calls[3]);
    std::fclose(file);
    std::fprintf(stderr,"[job-profile] completed=%llu retained=%zu; active loops excluded\n",total,snapshot.size());
}
#endif
