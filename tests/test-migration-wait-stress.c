/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <time.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

#define BURST_PAGES 200000
#define SPARSE_PAGES 64
#define TOTAL_PAGES (BURST_PAGES + SPARSE_PAGES)
static uint64_t submitted[TOTAL_PAGES], latency[TOTAL_PAGES];
static bool seen[TOTAL_PAGES];
static _Atomic unsigned int processed, poll_calls, balance_calls;

static uint64_t now_ns(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000000000ULL + time.tv_nsec;
}
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    atomic_fetch_add(&poll_calls, 1);
    return __real_poll(fds, count, timeout);
}
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t capacity)
{
    assert(count <= capacity / sizeof(*fds));
    return __wrap_poll(fds, count, timeout);
}
int pact_pin_to_cpu(int cpu)
{
    (void)cpu;
    assert(false);
    return -1;
}
void check_migration_balance(pact_context_t *ctx)
{
    (void)ctx;
    atomic_fetch_add(&balance_calls, 1);
}
int numa_move_pages(int pid, unsigned long count, void **pages, const int *nodes, int *status,
                    int flags)
{
    assert(pid == getpid() && count <= 32 && flags == MPOL_MF_MOVE);
    for (unsigned long i = 0; i < count; i++) {
        uintptr_t index = (uintptr_t)pages[i] / PAGE_SIZE - 1;
        assert(index < TOTAL_PAGES && nodes[i] == 0 && !seen[index]);
        seen[index] = true;
        latency[index] = now_ns() - submitted[index];
        status[i] = 0;
    }
    atomic_fetch_add(&processed, count);
    return 0;
}
static int compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}
int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    alarm(15);
    pac_metadata_t *metadata = calloc(TOTAL_PAGES, sizeof(*metadata));
    assert(metadata);
    pact_workload_t wl = {.target_pid = getpid()};
    pact_context_t ctx = {.workload = &wl,
                          .migration_cpu = -1,
                          .max_migrations_per_cycle = 32,
                          /* A short balance interval checks progress during idle waits. */
                          .tsc_freq_hz = 1};
    atomic_init(&ctx.migration_thread_running, false);
    assert(init_migration_thread(&ctx) == 0);
    for (unsigned int i = 0; i < TOTAL_PAGES; i++) {
        if (i >= BURST_PAGES) {
            while (atomic_load(&processed) != i) {
                sched_yield();
            }
            unsigned int before = atomic_load(&poll_calls);
            /* Wait for a new idle poll, including timeout-driven balancing. */
            while (atomic_load(&poll_calls) == before) {
                sched_yield();
            }
        }
        metadata[i].page_addr = (uint64_t)(i + 1) * PAGE_SIZE;
        metadata[i].tier = 1;
        metadata[i].migrating = true;
        submitted[i] = now_ns();
        migration_entry_t entry = {.meta = &metadata[i], .target_node = 0};
        while (!ring_buffer_migration_entry_push(wl.migration_ring, entry)) {
            sched_yield();
        }
        wake_migration_worker(&ctx);
    }
    while (atomic_load(&processed) != TOTAL_PAGES) {
        sched_yield();
    }
    unsigned int before = atomic_load(&poll_calls);
    while (atomic_load(&poll_calls) == before) {
        sched_yield();
    }
    uint64_t stop_start = now_ns();
    cleanup_migration_thread(&ctx);
    uint64_t stop_ns = now_ns() - stop_start;
    assert(!ctx.migration_thread_running && ctx.migration_wake_fd == -1);
    assert(!wl.migration_ring && !atomic_load(&ctx.migration_error));
    assert(wl.stats.promotion_successes == TOTAL_PAGES);
    assert(atomic_load(&balance_calls) >= SPARSE_PAGES);
    for (unsigned int i = 0; i < TOTAL_PAGES; i++) {
        assert(seen[i] && !metadata[i].migrating && metadata[i].tier == 0);
    }
    qsort(latency + BURST_PAGES, SPARSE_PAGES, sizeof(*latency), compare);
    printf("{\"pages\":%u,\"sparse_p50_ns\":%lu,\"sparse_max_ns\":%lu,"
           "\"shutdown_ns\":%lu,\"poll_calls\":%u,\"balance_calls\":%u}\n",
           TOTAL_PAGES, latency[BURST_PAGES + SPARSE_PAGES / 2], latency[TOTAL_PAGES - 1], stop_ns,
           atomic_load(&poll_calls), atomic_load(&balance_calls));
    free(metadata);
    alarm(0);
    return 0;
}
