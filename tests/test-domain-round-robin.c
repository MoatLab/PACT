/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* Migration batching. Tierce: one round-robin pass gives each domain one
 * numa_move_pages() call of at most --domain-migration-batch pages, with
 * the pages of that domain's workload only. PACT: the global queue drains
 * in calls that each hold the pages of one owner process. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

static struct {
    int pid;
    unsigned long count;
} calls[16];
static int nr_calls;

int numa_move_pages(int pid, unsigned long count, void **pages, const int *nodes, int *status,
                    int flags)
{
    if (!nodes) {
        /* The node query before each call: every queued page is on node 1. */
        assert(flags == 0 && count > 0);
        for (unsigned long i = 0; i < count; i++) {
            status[i] = 1;
        }
        return 0;
    }
    assert(flags == MPOL_MF_MOVE && nr_calls < 16);
    calls[nr_calls].pid = pid;
    calls[nr_calls].count = count;
    nr_calls++;
    for (unsigned long i = 0; i < count; i++) {
        assert(pages[i] && nodes[i] == 0);
        status[i] = 0; /* moved to the fast tier */
    }
    return 0;
}

static void queue(pac_domain_t *domain, pac_metadata_t *meta, pid_t pid, uint64_t page, int tier)
{
    meta->pid = pid;
    meta->page_addr = page * PAGE_SIZE;
    meta->tier = tier;
    meta->migrating = true;
    migration_entry_t entry = {.meta = meta, .target_node = 0};
    assert(ring_buffer_migration_entry_push(domain->migration_ring, entry));
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    pact_workload_t workloads[2] = {{.index = 0, .domain = 0, .target_pid = 100},
                                    {.index = 1, .domain = 1, .target_pid = 200}};
    pac_domain_t domains[2] = {{0}};
    pact_context_t ctx = {.mode = PACT_MODE_TIERCE,
                          .workloads = workloads,
                          .nr_workloads = 2,
                          .domains = domains,
                          .nr_domains = 2,
                          .domain_migration_batch = 2};
    assert(migration_batch_size(&ctx) == 2);
    assert(alloc_migration_rings(&ctx, 64) == 0);

    pac_metadata_t a[5] = {{0}}, b[3] = {{0}};
    for (int i = 0; i < 5; i++) {
        a[i].workload = 0;
        queue(&domains[0], &a[i], 100, (uint64_t)i + 1, 1);
    }
    for (int i = 0; i < 3; i++) {
        b[i].workload = 1;
        /* b[1] reached the fast tier before the worker saw it: skipped. */
        queue(&domains[1], &b[i], 200, (uint64_t)i + 10, i == 1 ? 0 : 1);
    }

    void *pages[2];
    int nodes[2], status[2];
    pac_metadata_t *metas[2];
    bool found;
    /* Pass 1: domain 0 moves 2 pages, domain 1 pops 2 entries and moves the
     * one still on the slow tier. */
    assert(mig_round_robin(&ctx, pages, nodes, status, metas, 2, &found) == 3 && found);
    assert(nr_calls == 2);
    assert(calls[0].pid == 100 && calls[0].count == 2);
    assert(calls[1].pid == 200 && calls[1].count == 1);
    assert(!b[1].migrating);
    /* Pass 2: domain 0 moves 2 more, domain 1 moves its last page. */
    assert(mig_round_robin(&ctx, pages, nodes, status, metas, 2, &found) == 3 && found);
    assert(nr_calls == 4 && calls[2].pid == 100 && calls[2].count == 2);
    assert(calls[3].pid == 200 && calls[3].count == 1);
    /* Pass 3: only domain 0 has work left. */
    assert(mig_round_robin(&ctx, pages, nodes, status, metas, 2, &found) == 1 && found);
    assert(nr_calls == 5 && calls[4].pid == 100 && calls[4].count == 1);
    /* Empty rings: nothing found. */
    assert(mig_round_robin(&ctx, pages, nodes, status, metas, 2, &found) == 0 && !found);

    assert(workloads[0].stats.promotion_successes == 5);
    assert(workloads[1].stats.promotion_successes == 2);
    for (int i = 0; i < 5; i++) {
        assert(!a[i].migrating && a[i].tier == 0);
    }
    free_migration_rings(&ctx);

    /* PACT keeps its per-call limit. */
    pact_context_t pact = {.max_migrations_per_cycle = 4096, .domain_migration_batch = 32};
    assert(migration_batch_size(&pact) == 4096);

    /* PACT drains its one global queue: each call holds the pages of one
     * owner process, at most max_batch of them. */
    pac_domain_t global = {0};
    pact.domains = &global;
    pact.nr_domains = 1;
    pact.workloads = workloads;
    pact.nr_workloads = 2;
    assert(alloc_migration_rings(&pact, 64) == 0);
    pac_metadata_t g[5] = {{0}};
    pid_t owners[5] = {100, 100, 100, 200, 100};
    for (int i = 0; i < 5; i++) {
        g[i].workload = owners[i] == 100 ? 0 : 1;
        queue(&global, &g[i], owners[i], (uint64_t)i + 20, 1);
    }
    nr_calls = 0;
    assert(mig_drain_ring(&pact, global.migration_ring, pages, nodes, status, metas, 2) == 5);
    assert(nr_calls == 4);
    assert(calls[0].pid == 100 && calls[0].count == 2);
    assert(calls[1].pid == 100 && calls[1].count == 1);
    assert(calls[2].pid == 200 && calls[2].count == 1);
    assert(calls[3].pid == 100 && calls[3].count == 1);
    free_migration_rings(&pact);
    return 0;
}
