/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

/* Require a real atomic object, not a volatile flag or a cast at one access. */
_Static_assert(_Generic(&((pact_context_t *)0)->migration_thread_running,
               _Atomic bool *: 1,
               default: 0),
               "migration stop flag must be atomic");

static _Atomic bool entered;
static _Atomic unsigned int processed;
static bool dispatch_enabled;

int pact_pin_to_cpu(int cpu)
{
    (void)cpu;
    assert(false);
    return -1;
}

void check_migration_balance(pact_context_t *ctx)
{
    (void)ctx;
    atomic_store(&entered, true);
}

int numa_move_pages(int pid, unsigned long count, void **pages, const int *nodes, int *status,
                    int flags)
{
    /* Exercise real queue dispatch and result handling without moving pages. */
    assert(dispatch_enabled);
    assert(pid == getpid() && count > 0 && count <= 2 && flags == MPOL_MF_MOVE);
    int failures = 0;
    for (unsigned long i = 0; i < count; i++) {
        uintptr_t index = (uintptr_t)pages[i] / PAGE_SIZE;
        assert(index >= 1 && index <= 4 && nodes[i] == 0);
        status[i] = index % 2 ? 0 : -EBUSY;
        failures += status[i] < 0;
    }
    atomic_fetch_add(&processed, count);
    return failures;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    /* Bound a shutdown failure without adding synchronization to the flag. */
    alarm(10);
    for (int i = 0; i < 100; i++) {
        pact_workload_t wl = {.target_pid = getpid()};
        pact_context_t ctx = {
            .workload = &wl, .migration_cpu = -1, .max_migrations_per_cycle = 1, .tsc_freq_hz = 1};
        atomic_init(&ctx.migration_thread_running, false);
        atomic_store(&entered, false);
        assert(init_migration_thread(&ctx) == 0);
        while (!atomic_load(&entered)) {
            sched_yield();
        }
        cleanup_migration_thread(&ctx);
        assert(!atomic_load(&ctx.migration_thread_running));
        assert(wl.migration_ring == NULL);
    }
    dispatch_enabled = true;
    for (int i = 0; i < 100; i++) {
        pact_workload_t wl = {.target_pid = getpid()};
        pact_context_t ctx = {
            .workload = &wl, .migration_cpu = -1, .max_migrations_per_cycle = 2, .tsc_freq_hz = 1};
        pac_metadata_t metadata[4] = {0};
        atomic_init(&ctx.migration_thread_running, false);
        atomic_store(&processed, 0);
        assert(init_migration_thread(&ctx) == 0);
        for (int page = 0; page < 4; page++) {
            metadata[page].page_addr = (page + 1) * PAGE_SIZE;
            metadata[page].tier = 1;
            metadata[page].migrating = true;
            migration_entry_t entry = {.meta = &metadata[page], .target_node = 0};
            assert(ring_buffer_migration_entry_push(wl.migration_ring, entry));
        }
        while (atomic_load(&processed) < 4) {
            sched_yield();
        }
        /* Join before reading metadata: result handling follows dispatch. */
        cleanup_migration_thread(&ctx);
        assert(!atomic_load(&ctx.migration_thread_running));
        assert(wl.migration_ring == NULL);
        assert(wl.stats.promotion_successes == 2 && wl.stats.promotion_failures == 2);
        for (int page = 0; page < 4; page++) {
            assert(!metadata[page].migrating);
            assert(metadata[page].tier == (page % 2 ? 1 : 0));
        }
    }
    alarm(0);
    return 0;
}
