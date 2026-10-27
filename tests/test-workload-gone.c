/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* PACT mode: a workload exits only when its pidfd reports the exit. A
 * counter refresh that finds every thread of a workload gone while its
 * pidfd has not fired keeps the workload: its groups close once, and the
 * next window refreshes it again. A fired pidfd marks the workload exited
 * and closes its groups once, the other workloads keep running, and the
 * run stops cleanly when every workload has exited. A real refresh error
 * still stops sampling. Runs the actual PEBS coroutine and the actual exit
 * helpers with real pidfds. Counter calls are stubs. */
#include "../src/pebs-aggregator.c"
#include "../src/utils.h"
#include <assert.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static pact_context_t *active;
static bool restarted;
static int setup_result[2], setups[2], cleanups[2];
static thread_perf_t dummy_thread;

void *mco_get_user_data(mco_coro *co)
{
    (void)co;
    return active;
}
mco_result mco_yield(mco_coro *co)
{
    (void)co;
    active->running = false; /* one window per call */
    return MCO_SUCCESS;
}
void stop_pmu_perf_events(pact_context_t *ctx)
{
    (void)ctx;
}
void read_pmu_counting_events(pact_context_t *ctx)
{
    (void)ctx;
}
void start_pmu_perf_events(pact_context_t *ctx)
{
    (void)ctx;
    restarted = true;
}
bool workload_covers_tid(const pact_workload_t *wl, pid_t tid)
{
    (void)wl;
    (void)tid;
    return true;
}

/* The refresh result of each workload: 0, PACT_WORKLOAD_GONE, or -1 (EIO).
 * A gone workload keeps its old groups here, as when its task directory is
 * gone, so the refresh must close them. */
int setup_workload_counting_events(pact_workload_t *wl)
{
    setups[wl->index]++;
    if (setup_result[wl->index] < 0) {
        errno = EIO;
    }
    return setup_result[wl->index];
}
void cleanup_workload_counting_events(pact_workload_t *wl)
{
    cleanups[wl->index]++;
    wl->threads = NULL;
    wl->nr_threads = 0;
}

/* Run one sampling window. */
static void window(pact_context_t *ctx)
{
    restarted = false;
    ctx->running = true;
    pebs_aggregator_coroutine(NULL);
}

static void reset(pact_workload_t *wl, int pidfd)
{
    for (int w = 0; w < 2; w++) {
        wl[w].exited = false;
        wl[w].threads = &dummy_thread;
        wl[w].nr_threads = 1;
        setup_result[w] = 0;
        setups[w] = 0;
        cleanups[w] = 0;
    }
    wl[0].pidfd = pidfd;
    wl[1].pidfd = pidfd;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    per_cpu_state_t cpu = {.fd_pebs = 1};
    cpu.pebs_mmap = calloc(1, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
    assert(cpu.pebs_mmap);
    /* Our own pidfd never fires while the test runs. */
    int alive = syscall(SYS_pidfd_open, getpid(), 0);
    assert(alive >= 0);
    pact_workload_t wl[2] = {{.target_pid = getpid(), .index = 0},
                             {.target_pid = getpid(), .index = 1}};
    pac_domain_t domain = {0};
    pact_context_t ctx = {.workloads = wl,
                          .nr_workloads = 2,
                          .domains = &domain,
                          .nr_domains = 1,
                          .global_mlp = 1,
                          .sampling_interval_ms = 10,
                          .tsc_freq_hz = 1000000000};
    active = &ctx;
    ctx.pac_update_ring = ring_buffer_pac_update_create(16);
    pebs_aggregator_t *agg = pebs_aggregator_create(&cpu, 1, 1);
    ctx.pebs_aggregator = agg;
    agg->pact_ctx = &ctx;

    /* Workload 0 loses every thread, but its pidfd does not fire. It stays,
     * its groups close once, and the run continues with both workloads. */
    reset(wl, alive);
    setup_result[0] = PACT_WORKLOAD_GONE;
    window(&ctx);
    assert(!wl[0].exited && !wl[1].exited);
    assert(cleanups[0] == 1 && cleanups[1] == 0 && !wl[0].threads);
    assert(restarted && !ctx.sampling_failed);

    /* Next window: workload 0 is refreshed again, and nothing closes twice. */
    window(&ctx);
    assert(!wl[0].exited && setups[0] == 2 && cleanups[0] == 1 && setups[1] == 2);
    assert(restarted && !ctx.sampling_failed);

    /* The pidfd path: workload 0's process exited and its pidfd fired. It is
     * marked exited before any refresh, its groups close once, and the run
     * continues with workload 1. */
    int gate[2];
    assert(!pipe(gate));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        char byte;
        close(gate[1]);
        _exit(read(gate[0], &byte, 1) == 1 ? 0 : 1);
    }
    close(gate[0]);
    int fired = syscall(SYS_pidfd_open, child, 0);
    assert(fired >= 0);
    assert(write(gate[1], "x", 1) == 1);
    close(gate[1]);
    siginfo_t info;
    assert(!waitid(P_PID, child, &info, WEXITED | WNOWAIT));
    reset(wl, alive);
    wl[0].pidfd = fired;
    window(&ctx);
    assert(wl[0].exited && !wl[1].exited);
    assert(setups[0] == 0 && cleanups[0] == 1 && cleanups[1] == 0);
    assert(restarted && !ctx.sampling_failed);

    /* Next window: workload 0 is not refreshed or closed again. */
    window(&ctx);
    assert(setups[0] == 0 && cleanups[0] == 1 && setups[1] == 2);
    assert(restarted && !ctx.sampling_failed);

    /* Workload 1 loses every thread and its pidfd fires too: every workload
     * has exited, so the run stops without a failure and without a counter
     * restart. The liveness check ends the window before any refresh, and
     * the groups of workload 1 close at exit (pact_destroy). */
    setup_result[1] = PACT_WORKLOAD_GONE;
    wl[1].pidfd = fired;
    window(&ctx);
    assert(wl[0].exited && wl[1].exited && setups[1] == 2 && cleanups[1] == 0);
    assert(!ctx.running && !restarted && !ctx.sampling_failed);
    int status;
    assert(waitpid(child, &status, 0) == child);
    close(fired);

    /* A real refresh error of a live workload still stops sampling. */
    reset(wl, alive);
    setup_result[0] = -1;
    window(&ctx);
    assert(!wl[0].exited && ctx.sampling_failed && !ctx.running && !restarted);

    close(alive);
    pebs_aggregator_destroy(agg);
    ring_buffer_pac_update_destroy(ctx.pac_update_ring);
    free(cpu.pebs_mmap);
    return 0;
}
