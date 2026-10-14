/* Standalone test of the real pump accounting headers.
 *
 * Build with a C++17 compiler, for example:
 *   clang++ -std=c++17 tests/test_pump_intervals.cpp -o test_pump_intervals
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* haze_pump_profile.h only needs this small Win32 surface. Mock it here so
 * accounting and ring behavior can be tested without a window or game run. */
using LONGLONG = long long;
using HANDLE = void*;
struct LARGE_INTEGER { LONGLONG QuadPart; };
struct FILETIME { std::uint32_t dwLowDateTime, dwHighDateTime; };

static LONGLONG g_mock_qpc = 0;
static LONGLONG g_mock_frequency = 1000;
static std::uint64_t g_mock_cpu_100ns = 0;
static bool g_mock_thread_times_valid = true;
static unsigned g_mock_qpc_calls = 0;

static int QueryPerformanceFrequency(LARGE_INTEGER* value)
{
    value->QuadPart = g_mock_frequency;
    return 1;
}

static int QueryPerformanceCounter(LARGE_INTEGER* value)
{
    ++g_mock_qpc_calls;
    value->QuadPart = g_mock_qpc;
    return 1;
}

static HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(1); }
static unsigned GetCurrentThreadId() { return 37; }

static FILETIME mock_filetime(std::uint64_t value)
{
    return FILETIME{static_cast<std::uint32_t>(value),
                    static_cast<std::uint32_t>(value >> 32)};
}

static int GetThreadTimes(HANDLE, FILETIME* creation, FILETIME* exit,
                          FILETIME* kernel, FILETIME* user)
{
    *creation = mock_filetime(0);
    *exit = mock_filetime(0);
    *kernel = mock_filetime(g_mock_cpu_100ns);
    *user = mock_filetime(0);
    return g_mock_thread_times_valid ? 1 : 0;
}

#ifndef _WIN32
#define _WIN32 1
#endif
#include "../port/haze_pump_profile.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool test_startup_partial_and_consecutive()
{
    auto* p = new HazePumpProfile();
    p->enabled = true;
    p->frequency = 1000;
    p->epoch = 0;
    p->wait_control = nullptr;
    p->csv_path = "pump_intervals_unused.csv";

    // Work before the first present is startup work and must not create a row.
    p->note_wait(true);
    p->add_wait(10);
    p->add_fifo(20);
    p->add_tick(3);
    g_mock_qpc = 1100;
    g_mock_cpu_100ns = 100000;
    p->finish_present(1000, 1, 1);
    CHECK(p->recorded == 0);
    CHECK(p->intervals.previous == 1100);
    CHECK(p->intervals.pending.wait == 0);
    CHECK(p->intervals.pending.fifo == 0);

    // Two drains and one wait accumulate until the next actual present.
    p->note_wait(false);
    p->add_wait(12);
    p->add_fifo(4);
    p->add_fifo(6);
    p->add_tick(3);
    g_mock_qpc = 1300;
    g_mock_cpu_100ns = 120000;
    p->finish_present(1260, 2, 2);
    CHECK(p->recorded == 1);
    const HazePumpInterval& partial = p->rows[0];
    CHECK(partial.begin == 1100 && partial.end == 1300);
    CHECK(partial.frame == 2 && partial.flips == 2);
    CHECK(partial.wait == 12 && partial.fifo == 10 && partial.tick == 3);
    CHECK(partial.present == 40);
    CHECK(partial.waits == 1 && partial.queued_waits == 0 && partial.drains == 2);
    CHECK(partial.cpu_valid && partial.cpu_100ns == 20000);

    // Consecutive pending-flip callbacks have a present scope but no new drain.
    g_mock_qpc = 1400;
    g_mock_cpu_100ns = 130000;
    p->finish_present(1350, 3, 1);
    CHECK(p->recorded == 2);
    const HazePumpInterval& consecutive = p->rows[1];
    CHECK(consecutive.begin == 1300 && consecutive.end == 1400);
    CHECK(consecutive.frame == 3 && consecutive.flips == 1);
    CHECK(consecutive.wait == 0 && consecutive.fifo == 0 && consecutive.tick == 0);
    CHECK(consecutive.present == 50 && consecutive.drains == 0 && consecutive.waits == 0);
    CHECK(consecutive.cpu_valid && consecutive.cpu_100ns == 10000);

    // Model a pre-drain pending present, then a drain that reaches another flip.
    p->add_fifo(8);
    g_mock_qpc = 1500;
    g_mock_cpu_100ns = 140000;
    p->finish_present(1450, 4, 1);
    CHECK(p->recorded == 3 && p->rows[2].fifo == 8 && p->rows[2].drains == 1);
    p->add_fifo(9);
    g_mock_qpc = 1600;
    g_mock_cpu_100ns = 150000;
    p->finish_present(1550, 5, 1);
    CHECK(p->recorded == 4 && p->rows[3].fifo == 9 && p->rows[3].drains == 1);

    delete p;
    return true;
}

static bool test_cpu_validity_and_recovery()
{
    HazePumpIntervals intervals;
    HazePumpInterval row{};
    CHECK(!intervals.finish(100, 10, true, 1, 1, row)); // first sample is startup
    CHECK(intervals.finish(200, 20, false, 2, 1, row)); // invalid current sample
    CHECK(!row.cpu_valid && row.cpu_100ns == 0);
    CHECK(intervals.finish(300, 40, true, 3, 1, row)); // prior sample invalid
    CHECK(!row.cpu_valid);
    CHECK(intervals.finish(400, 90, true, 4, 1, row));
    CHECK(row.cpu_valid && row.cpu_100ns == 50);
    CHECK(intervals.finish(500, 70, true, 5, 1, row)); // counter moved backward
    CHECK(!row.cpu_valid && row.cpu_100ns == 0);
    CHECK(intervals.finish(600, 120, true, 6, 1, row)); // recovers from new baseline
    CHECK(row.cpu_valid && row.cpu_100ns == 50);
    return true;
}

static bool test_report_reset_is_independent()
{
    auto* p = new HazePumpProfile();
    p->enabled = true;
    p->frequency = 1000;
    p->epoch = 0;
    p->wait_control = nullptr;
    p->csv_path = "pump_intervals_unused.csv";
    p->add_wait(7);
    p->add_fifo(11);
    p->add_tick(13);
    p->note_wait(true);
    p->add_fifo(6);

    g_mock_qpc = 2000;
    p->report();
    CHECK(p->wait == 0 && p->fifo == 0 && p->tick == 0);
    CHECK(p->drains == 0 && p->waits == 0 && p->queued_waits == 0);
    CHECK(p->intervals.pending.fifo == 17);
    CHECK(p->intervals.pending.drains == 2);

    delete p;
    return true;
}

static bool test_ring_wrap_and_dump()
{
    const char* path = "pump_intervals_ring_test.csv";
    auto* p = new HazePumpProfile();
    p->enabled = true;
    p->frequency = 1000;
    p->csv_path = path;
    p->wait_control = nullptr;
    p->recorded = 0;
    p->intervals.previous = 1;
    p->intervals.previous_cpu = 1000;
    p->intervals.previous_cpu_valid = true;

    const unsigned total = HazePumpProfile::capacity + 2;
    for (unsigned i = 0; i < total; ++i) {
        g_mock_qpc = 2000 + static_cast<LONGLONG>(i) * 20;
        g_mock_cpu_100ns += 1000;
        p->add_fifo(2);
        p->finish_present(g_mock_qpc - 10, i + 1, i % 4);
    }
    CHECK(p->recorded == total);
    CHECK(p->rows[2].frame == 3);
    CHECK(p->rows[1].frame == total);
    p->dump();

    FILE* csv = std::fopen(path, "rb");
    CHECK(csv != nullptr);
    char line[512];
    CHECK(std::fgets(line, sizeof(line), csv) != nullptr); // header
    CHECK(std::fgets(line, sizeof(line), csv) != nullptr);
    long long frequency = 0, begin = 0, end = 0;
    unsigned frame = 0;
    CHECK(std::sscanf(line, "%lld,%u,%*u,%lld,%lld", &frequency, &frame,
                      &begin, &end) == 4);
    CHECK(frequency == 1000 && frame == 3);
    unsigned rows = 1;
    unsigned last_frame = frame;
    while (std::fgets(line, sizeof(line), csv)) {
        CHECK(std::sscanf(line, "%lld,%u", &frequency, &last_frame) == 2);
        ++rows;
    }
    std::fclose(csv);
    std::remove(path);
    std::remove("pump_intervals_ring_test.csv.waits.csv");
    CHECK(rows == HazePumpProfile::capacity);
    CHECK(last_frame == total);

    delete p;
    return true;
}

static bool test_wait_outcomes_partition_and_reset()
{
    auto* p = new HazePumpProfile();
    p->enabled = true;
    p->frequency = 1000;
    p->wait_control = nullptr;
    p->csv_path = "pump_intervals_unused.csv";
    p->intervals.previous = 100;
    // Two real signal returns, one with the queue empty after wake. An empty
    // snapshot does not establish whether a semaphore/label was published.
    const unsigned outcomes[] = {0, 0, 1, 2, 3, 99};
    const LONGLONG durations[] = {2, 3, 10, 7, 1, 4};
    for (unsigned i=0; i<6; ++i) {
        p->note_wait(i==0);
        p->add_wait(durations[i]);
        p->note_wait_result(outcomes[i],durations[i],i==0 || i==2);
    }
    g_mock_qpc = 200;
    p->finish_present(190,2,2);
    CHECK(p->recorded==1);
    const auto& row=p->rows[0];
    CHECK(row.waits==6 && row.wait==27 && row.queued_waits==1);
    CHECK(row.signaled_waits==2 && row.signaled_wait==5);
    CHECK(row.timeout_waits==1 && row.timeout_wait==10);
    CHECK(row.sleep_waits==1 && row.sleep_wait==7);
    CHECK(row.failed_waits==2 && row.failed_wait==5);
    CHECK(row.wait==row.signaled_wait+row.timeout_wait+row.sleep_wait+row.failed_wait);
    CHECK(row.max_wait==10 && row.queued_after_waits==2 && row.empty_signals==1);
    CHECK(p->intervals.pending.waits==0 && p->intervals.pending.max_wait==0);
    CHECK(p->intervals.pending.signaled_waits==0 && p->intervals.pending.empty_signals==0);
    // Disabled CSV reporting must not accumulate diagnostic outcomes.
    p->csv_path=nullptr;
    p->note_wait_result(0,999,true);
    CHECK(p->intervals.pending.signaled_waits==0 && p->intervals.pending.max_wait==0);
    delete p;
    return true;
}

static bool test_wait_mode_reporting_clocks_only_on_change()
{
    auto* p = new HazePumpProfile();
    p->enabled=false;
    const auto before=g_mock_qpc_calls;
    p->report_wait_mode();
    CHECK(p->reported_wait_ms==-1 && g_mock_qpc_calls==before);
    p->enabled=true;
    p->report_wait_mode();
    CHECK(p->reported_wait_ms==1 && g_mock_qpc_calls==before+1);
    p->report_wait_mode();
    CHECK(g_mock_qpc_calls==before+1);
    p->wait_ms=0;
    p->report_wait_mode();
    CHECK(p->reported_wait_ms==0 && g_mock_qpc_calls==before+2);
    delete p;
    return true;
}

static bool test_environment_paths_are_owned()
{
    const char* csv_path="pump_intervals_owned_path.csv";
    const char* control_path="pump_intervals_owned_control.txt";
    const char* replacement_csv="pump_intervals_replacement.csv";
    const char* replacement_control="pump_intervals_replacement_control.txt";
    std::remove(csv_path);
    std::remove((std::string(csv_path)+".waits.csv").c_str());
    std::remove(control_path);
    CHECK(_putenv_s("HAZE_PUMP_CSV",csv_path)==0);
    CHECK(_putenv_s("HAZE_FIFO_WAIT_FILE",control_path)==0);
    CHECK(_putenv_s("HAZE_PUMP_PROFILE","")==0);
    auto* p=new HazePumpProfile();
    CHECK(p->enabled && p->csv_path && p->wait_control);
    CHECK(std::strcmp(p->csv_path,csv_path)==0);
    CHECK(std::strcmp(p->wait_control,control_path)==0);

    // Later CRT environment changes may relocate the environment block. The
    // profile must retain copies for both its export path and live control.
    CHECK(_putenv_s("HAZE_PUMP_CSV",replacement_csv)==0);
    CHECK(_putenv_s("HAZE_FIFO_WAIT_FILE",replacement_control)==0);
    CHECK(std::strcmp(p->csv_path,csv_path)==0);
    CHECK(std::strcmp(p->wait_control,control_path)==0);

    FILE* control=std::fopen(control_path,"wb");
    CHECK(control!=nullptr);
    std::fputs("0\n",control);
    std::fclose(control);
    p->frequency=1000;
    p->epoch=0;
    p->wait_ms=1;
    g_mock_qpc=2000;
    p->report();
    CHECK(p->wait_ms==0); // read the original control file after env mutation

    p->intervals.previous=100;
    p->intervals.previous_cpu=1000;
    p->intervals.previous_cpu_valid=true;
    g_mock_qpc=200;
    g_mock_cpu_100ns=2000;
    p->add_fifo(10);
    p->finish_present(150,1,1);
    CHECK(p->recorded==1);
    p->dump();
    FILE* csv=std::fopen(csv_path,"rb");
    CHECK(csv!=nullptr);
    char line[1024];
    CHECK(std::fgets(line,sizeof(line),csv)!=nullptr);
    CHECK(std::fgets(line,sizeof(line),csv)!=nullptr);
    std::fclose(csv);
    std::remove(csv_path);
    std::remove((std::string(csv_path)+".waits.csv").c_str());
    std::remove(control_path);
    delete p;
    CHECK(_putenv_s("HAZE_PUMP_CSV","")==0);
    CHECK(_putenv_s("HAZE_FIFO_WAIT_FILE","")==0);
    return true;
}

static bool test_slow_wait_retention()
{
    auto* p=new HazePumpProfile();
    p->csv_path=nullptr;
    p->record_wait(0,10,1,false,true);
    CHECK(p->wait_sequence==0 && p->slow_wait_count==0);
    p->csv_path="pump_slow_wait_test.csv";
    p->frequency=1000;
    p->record_wait(0,2,0,false,false); // exactly 2 ms is not retained
    CHECK(p->wait_sequence==1 && p->slow_wait_count==0);
    const unsigned total=HazePumpProfile::capacity+2;
    for(unsigned i=0;i<total;++i)
        p->record_wait(10+i*4,13+i*4,i%4,i%2!=0,i%2==0);
    CHECK(p->slow_wait_count==total && p->wait_sequence==total+1);
    CHECK(p->last_discarded_wait_end==17);
    p->dump_waits();
    FILE* csv=std::fopen("pump_slow_wait_test.csv.waits.csv","rb");
    CHECK(csv!=nullptr);
    char line[512];
    CHECK(std::fgets(line,sizeof(line),csv)!=nullptr);
    CHECK(std::strstr(line,"overwritten,2,max_discarded_end_qpc,17")!=nullptr);
    CHECK(std::fgets(line,sizeof(line),csv)!=nullptr); // columns
    unsigned rows=0;
    while(std::fgets(line,sizeof(line),csv)) {
        unsigned long long sequence;
        long long begin,end;
        unsigned tid,timeout,result,before,after;
        CHECK(std::sscanf(line,"%llu,%lld,%lld,%u,%u,%u,%u,%u",
            &sequence,&begin,&end,&tid,&timeout,&result,&before,&after)==8);
        const unsigned i=rows+2;
        CHECK(sequence==i+2 && begin==10+i*4 && end==13+i*4);
        CHECK(tid==37 && timeout==1 && result==i%4);
        CHECK(before==i%2 && after==1-i%2);
        ++rows;
    }
    std::fclose(csv);
    std::remove("pump_slow_wait_test.csv.waits.csv");
    CHECK(rows==HazePumpProfile::capacity);
    delete p;
    return true;
}

int main()
{
    if (!test_startup_partial_and_consecutive()) return 1;
    if (!test_cpu_validity_and_recovery()) return 1;
    if (!test_report_reset_is_independent()) return 1;
    if (!test_ring_wrap_and_dump()) return 1;
    if (!test_wait_outcomes_partition_and_reset()) return 1;
    if (!test_wait_mode_reporting_clocks_only_on_change()) return 1;
    if (!test_environment_paths_are_owned()) return 1;
    if (!test_slow_wait_retention()) return 1;
    std::puts("pump interval accounting: PASS");
    return 0;
}
