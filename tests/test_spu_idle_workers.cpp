/*
 * Windows-only regression test for the SPU idle-worker cache core.
 * Example: clang-cl /std:c++17 /EHsc /DSPU_IDLE_WORKERS_TESTING
 *   /DSPU_IDLE_WORKER_SLOTS=2 /DSPU_IDLE_WORKER_TIMEOUT_MS=250
 *   /I external/ps3recomp/runtime/spu tests/test_spu_idle_workers.cpp
 *   /Fe:spu-idle-workers-test.exe
 */
#define SPU_IDLE_WORKERS_TESTING 1
#define SPU_IDLE_WORKER_SLOTS 2
#define SPU_IDLE_WORKER_TIMEOUT_MS 250
#include "../external/ps3recomp/runtime/spu/spu_idle_workers.h"

#include <stdio.h>

static int g_failures;

#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++g_failures; } \
} while (0)

typedef struct test_job {
    volatile LONG calls;
    HANDLE started;
    HANDLE release;
    HANDLE done;
    DWORD thread_id;
} test_job;

static void init_job(test_job* job, int block)
{
    job->calls = 0;
    job->started = CreateEventW(NULL, TRUE, FALSE, NULL);
    job->release = block ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
    job->done = CreateEventW(NULL, TRUE, FALSE, NULL);
}

static void destroy_job(test_job* job)
{
    if (job->started) CloseHandle(job->started);
    if (job->release) CloseHandle(job->release);
    if (job->done) CloseHandle(job->done);
    job->started = job->release = job->done = NULL;
}

static void test_callback(void* opaque)
{
    test_job* job = (test_job*)opaque;
    InterlockedIncrement(&job->calls);
    job->thread_id = GetCurrentThreadId();
    SetEvent(job->started);
    if (job->release) WaitForSingleObject(job->release, 10000);
    SetEvent(job->done);
}

static int wait_state_count(enum spu_idle_worker_state state, size_t wanted,
                            DWORD timeout_ms)
{
    const DWORD start = GetTickCount();
    do {
        size_t count = 0;
        AcquireSRWLockExclusive(&s_spu_idle_workers_lock);
        for (size_t i = 0; i < SPU_IDLE_WORKER_SLOTS; ++i)
            count += s_spu_idle_workers[i].state == state;
        ReleaseSRWLockExclusive(&s_spu_idle_workers_lock);
        if (count >= wanted) return 1;
        Sleep(1);
    } while ((DWORD)(GetTickCount() - start) < timeout_ms);
    return 0;
}

typedef struct nested_job {
    test_job parent;
    test_job* child;
    volatile LONG child_submit_ok;
    volatile LONG child_reused;
} nested_job;

static void nested_parent_callback(void* opaque)
{
    nested_job* job = (nested_job*)opaque;
    InterlockedIncrement(&job->parent.calls);
    job->parent.thread_id = GetCurrentThreadId();
    SetEvent(job->parent.started);
    int reused = 0;
    const int accepted = spu_idle_worker_submit(test_callback, job->child, &reused);
    InterlockedExchange(&job->child_reused, reused);
    InterlockedExchange(&job->child_submit_ok, accepted);
    if (accepted) WaitForSingleObject(job->child->done, 10000);
    SetEvent(job->parent.done);
}

enum { STRESS_SUBMITTERS = 8, STRESS_PER_SUBMITTER = 100 };
static volatile LONG g_stress_completed;
static HANDLE g_stress_all_done;

typedef struct stress_job {
    volatile LONG calls;
} stress_job;

typedef struct stress_submitter {
    stress_job* jobs;
    unsigned first;
    volatile LONG accepted;
} stress_submitter;

static void stress_callback(void* opaque)
{
    stress_job* job = (stress_job*)opaque;
    InterlockedIncrement(&job->calls);
    if (InterlockedIncrement(&g_stress_completed) ==
        STRESS_SUBMITTERS * STRESS_PER_SUBMITTER)
        SetEvent(g_stress_all_done);
}

static DWORD WINAPI stress_submitter_main(LPVOID opaque)
{
    stress_submitter* submitter = (stress_submitter*)opaque;
    for (unsigned i = 0; i < STRESS_PER_SUBMITTER; ++i) {
        int reused;
        if (spu_idle_worker_submit(stress_callback,
                                   &submitter->jobs[submitter->first + i],
                                   &reused))
            InterlockedIncrement(&submitter->accepted);
    }
    return 0;
}

typedef struct race_submit {
    test_job* job;
    HANDLE submitted;
    volatile LONG accepted;
    volatile LONG reused;
} race_submit;

static DWORD WINAPI race_submit_main(LPVOID opaque)
{
    race_submit* race = (race_submit*)opaque;
    int reused = 0;
    const int accepted = spu_idle_worker_submit(test_callback, race->job, &reused);
    InterlockedExchange(&race->reused, reused);
    InterlockedExchange(&race->accepted, accepted);
    SetEvent(race->submitted);
    return 0;
}

int main(void)
{
    /* Deterministically exercise failed startup before the cache has slots. */
    test_job failed;
    init_job(&failed, 0);
    spu_idle_workers_test_fail_next_create();
    int reused = -1;
    const int rejected = spu_idle_worker_submit(test_callback, &failed, &reused);
    CHECK(!rejected && reused == 0 && failed.calls == 0,
          "create failure leaves callback argument caller-owned");
    reused = -1;
    CHECK(!spu_idle_worker_submit(NULL, &failed, &reused) &&
          reused == 0 && failed.calls == 0,
          "null callback is rejected without taking argument ownership");
    destroy_job(&failed);

    /* First dispatch creates a reusable worker; the next one must run on it. */
    test_job first, second;
    init_job(&first, 0);
    init_job(&second, 0);
    reused = -1;
    CHECK(spu_idle_worker_submit(test_callback, &first, &reused) && !reused,
          "first submit creates worker");
    CHECK(WaitForSingleObject(first.done, 5000) == WAIT_OBJECT_0,
          "first callback completes");
    CHECK(wait_state_count(SPU_IDLE_SLOT_IDLE, 1, 5000),
          "worker publishes idle state after callback");
    const DWORD first_thread = first.thread_id;

    reused = 0;
    CHECK(spu_idle_worker_submit(test_callback, &second, &reused) && reused,
          "serial submit reuses idle worker");
    CHECK(WaitForSingleObject(second.done, 5000) == WAIT_OBJECT_0 &&
          second.thread_id == first_thread && first.calls == 1 && second.calls == 1,
          "reused worker runs each callback exactly once on the same thread");
    CHECK(wait_state_count(SPU_IDLE_SLOT_IDLE, 1, 5000),
          "reused worker returns to idle");

    /* Occupy both retained slots, then verify overflow and a nested dependent
     * submit still get fresh threads instead of waiting in a work queue. */
    test_job blocker1, blocker2, overflow_child;
    init_job(&blocker1, 1);
    init_job(&blocker2, 1);
    init_job(&overflow_child, 0);
    int r1 = 0, r2 = 1;
    const int a1 = spu_idle_worker_submit(test_callback, &blocker1, &r1);
    CHECK(a1 && r1, "first concurrent job takes idle worker");
    const int a2 = spu_idle_worker_submit(test_callback, &blocker2, &r2);
    CHECK(a2 && !r2, "second concurrent job creates another retained worker");
    CHECK(WaitForSingleObject(blocker1.started, 5000) == WAIT_OBJECT_0 &&
          WaitForSingleObject(blocker2.started, 5000) == WAIT_OBJECT_0,
          "both retained workers run concurrently");

    test_job failed_overflow;
    init_job(&failed_overflow, 0);
    spu_idle_workers_test_fail_next_create();
    int failed_reused = -1;
    CHECK(!spu_idle_worker_submit(test_callback, &failed_overflow, &failed_reused) &&
          failed_reused == 0 && failed_overflow.calls == 0,
          "busy-slot overflow creation failure leaves argument caller-owned");
    destroy_job(&failed_overflow);

    nested_job nested;
    init_job(&nested.parent, 0);
    nested.child = &overflow_child;
    nested.child_submit_ok = 0;
    nested.child_reused = -1;
    int r3 = 1;
    const int a3 = spu_idle_worker_submit(nested_parent_callback, &nested, &r3);
    CHECK(a3 && !r3, "busy-slot overflow starts detached one-shot thread");
    CHECK(WaitForSingleObject(nested.parent.done, 5000) == WAIT_OBJECT_0 &&
          WaitForSingleObject(overflow_child.done, 5000) == WAIT_OBJECT_0,
          "nested dependent submit progresses while retained workers are busy");
    CHECK(nested.child_submit_ok == 1 && nested.child_reused == 0 &&
          blocker1.calls == 1 && blocker2.calls == 1 && nested.parent.calls == 1 &&
          overflow_child.calls == 1,
          "overflow and nested callbacks each execute exactly once");

    SetEvent(blocker1.release);
    SetEvent(blocker2.release);
    CHECK(WaitForSingleObject(blocker1.done, 5000) == WAIT_OBJECT_0 &&
          WaitForSingleObject(blocker2.done, 5000) == WAIT_OBJECT_0,
          "blocked callbacks finish after release");
    CHECK(wait_state_count(SPU_IDLE_SLOT_IDLE, 2, 5000),
          "both retained slots become idle");

    /* Idle expiry releases slots; the next submission creates a new worker. */
    CHECK(wait_state_count(SPU_IDLE_SLOT_EMPTY, 2, 5000),
          "idle workers retire after timeout");
    test_job after_timeout;
    init_job(&after_timeout, 0);
    int after_reuse = 1;
    CHECK(spu_idle_worker_submit(test_callback, &after_timeout, &after_reuse) &&
          !after_reuse,
          "submit after retirement creates a fresh worker");
    CHECK(WaitForSingleObject(after_timeout.done, 5000) == WAIT_OBJECT_0 &&
          after_timeout.calls == 1,
          "fresh worker executes callback once");

    /* Race submissions against worker retirement. Either outcome is valid:
     * the submit wins idle reuse, or retirement frees the slot and submit
     * creates a replacement. Every accepted callback must still run once. */
    int race_pass = 1;
    for (int i = 0; i < 8; ++i) {
        test_job raced;
        init_job(&raced, 0);
        CHECK(spu_idle_worker_submit(test_callback, &raced, NULL),
              "retirement-race seed submit accepted");
        if (WaitForSingleObject(raced.done, 5000) != WAIT_OBJECT_0 ||
            !wait_state_count(SPU_IDLE_SLOT_IDLE, 1, 5000)) {
            race_pass = 0;
            destroy_job(&raced);
            break;
        }
        Sleep(SPU_IDLE_WORKER_TIMEOUT_MS - 5);
        race_submit race = {};
        race.job = &raced;
        race.submitted = CreateEventW(NULL, TRUE, FALSE, NULL);
        ResetEvent(raced.done);
        HANDLE submitter = race.submitted
            ? CreateThread(NULL, 0, race_submit_main, &race, 0, NULL) : NULL;
        if (!race.submitted || !submitter ||
            WaitForSingleObject(race.submitted, 5000) != WAIT_OBJECT_0) {
            race_pass = 0;
        } else {
            race_pass &= race.accepted == 1;
            race_pass &= WaitForSingleObject(raced.done, 5000) == WAIT_OBJECT_0;
            race_pass &= raced.calls == 2;
        }
        if (submitter) {
            race_pass &= WaitForSingleObject(submitter, 5000) == WAIT_OBJECT_0;
            CloseHandle(submitter);
        }
        if (race.submitted) CloseHandle(race.submitted);
        destroy_job(&raced);
    }
    CHECK(race_pass, "retirement racing submit loses no callback and creates no duplicate");

    /* Multiple submitters concurrently transfer static-lifetime arguments;
     * wait for every callback before inspecting or releasing the backing array. */
    const unsigned stress_count = STRESS_SUBMITTERS * STRESS_PER_SUBMITTER;
    stress_job* stress_jobs = (stress_job*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(stress_job) * stress_count);
    stress_submitter submitters[STRESS_SUBMITTERS] = {};
    HANDLE submitter_threads[STRESS_SUBMITTERS] = {};
    g_stress_completed = 0;
    g_stress_all_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    int stress_started = stress_jobs && g_stress_all_done;
    unsigned submitter_count = 0;
    for (unsigned i = 0; i < STRESS_SUBMITTERS && stress_started; ++i) {
        submitters[i].jobs = stress_jobs;
        submitters[i].first = i * STRESS_PER_SUBMITTER;
        submitter_threads[i] = CreateThread(NULL, 0, stress_submitter_main,
                                             &submitters[i], 0, NULL);
        if (submitter_threads[i]) ++submitter_count;
        else stress_started = 0;
    }
    unsigned accepted_total = 0;
    for (unsigned i = 0; i < STRESS_SUBMITTERS; ++i) {
        if (submitter_threads[i]) {
            if (WaitForSingleObject(submitter_threads[i], 30000) != WAIT_OBJECT_0)
                stress_started = 0;
            accepted_total += (unsigned)submitters[i].accepted;
        }
    }
    const DWORD stress_start = GetTickCount();
    while (stress_jobs && g_stress_completed < (LONG)accepted_total &&
           (DWORD)(GetTickCount() - stress_start) < 30000)
        Sleep(1);
    int stress_exact_once = stress_started && submitter_count == STRESS_SUBMITTERS &&
        accepted_total == stress_count && g_stress_completed == (LONG)accepted_total;
    for (unsigned i = 0; i < STRESS_SUBMITTERS; ++i)
        stress_exact_once &= submitters[i].accepted == STRESS_PER_SUBMITTER;
    for (unsigned i = 0; stress_jobs && i < stress_count; ++i)
        stress_exact_once &= stress_jobs[i].calls == 1;
    CHECK(stress_exact_once,
          "8 simultaneous submitters transfer 800 stable arguments exactly once");
    for (unsigned i = 0; i < STRESS_SUBMITTERS; ++i)
        if (submitter_threads[i]) CloseHandle(submitter_threads[i]);
    if (g_stress_all_done) CloseHandle(g_stress_all_done);
    if (stress_jobs) HeapFree(GetProcessHeap(), 0, stress_jobs);

    destroy_job(&first);
    destroy_job(&second);
    destroy_job(&blocker1);
    destroy_job(&blocker2);
    destroy_job(&overflow_child);
    destroy_job(&nested.parent);
    destroy_job(&after_timeout);
    return g_failures ? 1 : 0;
}
