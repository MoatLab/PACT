/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main
static int scenario, queries, prequeries, moves;
static bool prequery_done;
int numa_move_pages(int pid, unsigned long count, void **pages, const int *nodes, int *status,
                    int flags)
{
    assert(pid == 123);
    if (!nodes && !prequery_done && scenario < 99) {
        /* The node query before the promotion call. */
        prequery_done = true;
        prequeries++;
        assert(count == 3 && flags == 0);
        for (unsigned long i = 0; i < count; i++) {
            assert(status[i] == INT_MIN);
        }
        if (scenario == 11) {
            errno = ESRCH; /* the query fails as a whole */
            return -1;
        }
        if (scenario == 10) {
            status[0] = 0;       /* already on the fast tier */
            status[1] = -ENOENT; /* no node */
            status[2] = 1;
            return 0;
        }
        status[0] = status[1] = status[2] = 1;
        return 0;
    }
    if (scenario == 10) {
        /* Only the page on node 1 is sent, and the call moves it. */
        assert(nodes && count == 1 && flags == MPOL_MF_MOVE && pages[0] == (void *)12288);
        moves++;
        status[0] = 0;
        return 0;
    }
    assert(scenario != 11 && scenario != 12);
    if (scenario == 99) {
        assert(!nodes && count > 0 && count <= 256 && flags == 0);
        queries++;
        for (unsigned long i = 0; i < count; i++) {
            assert(status[i] == INT_MIN);
            status[i] = 0;
        }
        return 0;
    }

    if (nodes) {
        moves++;
        assert(count == 3 && flags == MPOL_MF_MOVE);
        status[1] = -EBUSY; /* Preserve this kernel-written failure. */
        if (scenario == 4) {
            status[0] = status[2] = 0;
            return 0;
        }
        errno = ENOMEM;
        return scenario == 0 ? 2 : -1;
    }
    queries++;
    assert(count == 2 && flags == 0);
    assert(pages[0] == (void *)4096 && pages[1] == (void *)12288);
    assert(status[0] == INT_MIN && status[1] == INT_MIN);
    if (scenario == 2) {
        status[0] = 0; /* Do not adopt writes after a failed query. */
        errno = EACCES;
        return -1;
    }
    status[0] = 0;
    if (scenario != 3) {
        status[1] = 1;
    }
    return 0;
}
int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    for (scenario = 0; scenario < 5; scenario++) {
        pact_workload_t wl = {0};
        pact_context_t ctx = {.workloads = &wl, .nr_workloads = 1};
        pac_metadata_t meta[3] = {{.tier = 1, .migrating = true},
                                  {.tier = 1, .migrating = true},
                                  {.tier = 1, .migrating = true}};
        pac_metadata_t *metas[] = {&meta[0], &meta[1], &meta[2]};
        void *pages[] = {(void *)4096, (void *)8192, (void *)12288};
        int nodes[] = {0, 0, 0}, status[3] = {-1, -1, -1};
        queries = prequeries = moves = 0;
        prequery_done = false;
        mig_dispatch_batch(&ctx, 123, pages, nodes, status, metas, 3);
        assert(prequeries == 1 && moves == 1);
        assert(queries == (scenario == 4 ? 0 : 1));
        assert(status[1] == -EBUSY && meta[1].tier == 1);
        assert(meta[0].tier == (scenario == 2 ? 1 : 0));
        assert(meta[2].tier == (scenario == 4 ? 0 : 1));
        assert(!meta[0].migrating && !meta[1].migrating && !meta[2].migrating);
        assert(wl.stats.promotion_successes == (scenario == 4 ? 2 : scenario == 2 ? 0 : 1));
        assert(wl.stats.promotion_failures + wl.stats.promotion_successes == 3);
    }

    /* The node query before the call. Page 0 is already on the fast
     * tier: it gets tier 0, is not sent and is no promotion. Page 1 has no
     * node: not sent, a failure. Page 2 is sent, moved and counted. */
    for (scenario = 10; scenario <= 12; scenario++) {
        pact_workload_t wl = {0};
        pact_context_t ctx = {.workloads = &wl, .nr_workloads = 1};
        pac_metadata_t meta[3] = {{.tier = 1, .migrating = true},
                                  {.tier = 1, .migrating = true},
                                  {.tier = 1, .migrating = true}};
        pac_metadata_t *metas[] = {&meta[0], &meta[1], &meta[2]};
        void *pages[] = {(void *)4096, (void *)8192, (void *)12288};
        int nodes[] = {0, 0, 0}, status[3] = {-1, -1, -1};
        queries = prequeries = moves = 0;
        prequery_done = false;
        atomic_store(&wl.exited, scenario == 12); /* 12: the workload has exited */
        mig_dispatch_batch(&ctx, 123, pages, nodes, status, metas, 3);
        assert(!meta[0].migrating && !meta[1].migrating && !meta[2].migrating);
        if (scenario == 10) {
            assert(prequeries == 1 && moves == 1 && queries == 0);
            assert(meta[0].tier == 0 && meta[1].tier == 1 && meta[2].tier == 0);
            assert(wl.stats.promotion_skips_resident == 1);
            assert(wl.stats.promotion_failures == 1 && wl.stats.promotion_successes == 1);
        } else if (scenario == 11) {
            /* A failed query sends nothing; every page stays retryable. */
            assert(prequeries == 1 && moves == 0 && ctx.residency_query_failures == 1);
            assert(meta[0].tier == 1 && meta[1].tier == 1 && meta[2].tier == 1);
            assert(wl.stats.promotion_failures == 3 && wl.stats.promotion_successes == 0);
        } else {
            /* An exited workload's pages are not sent and not counted. */
            assert(prequeries == 0 && moves == 0);
            assert(wl.stats.promotion_failures == 0 && wl.stats.promotion_successes == 0);
        }
    }
    /* Bounded stack batches must also handle configurable large migrations. */
    void *pages[513];
    int nodes[513], status[513];
    for (int i = 0; i < 513; i++) {
        pages[i] = (void *)(uintptr_t)((i + 1) * 4096);
        nodes[i] = 0;
        status[i] = INT_MIN;
    }
    scenario = 99;
    queries = 0;
    recover_migration_status(123, pages, nodes, status, 513);
    assert(queries == 3);
    for (int i = 0; i < 513; i++) {
        assert(status[i] == 0);
    }
    recover_migration_status(123, pages, nodes, status, 513);
    recover_migration_status(123, pages, nodes, status, 0);
    assert(queries == 3);
}
