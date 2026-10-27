/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <float.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

static void check_admission(uint64_t score, double width, bool expected)
{
    double samples[100] = {0};
    reservoir_t reservoir = {.samples = samples, .capacity = 100};
    binning_state_t bin = {.bin_count = 20, .bin_width = width};
    pac_domain_t domain = {.reservoir = &reservoir, .binning = &bin};
    domain.table = pac_table_init();
    domain.migration_ring = ring_buffer_migration_entry_create(8);
    assert(domain.migration_ring);
    pact_workload_t wl = {.target_pid = 123};
    pact_context_t ctx = {.workloads = &wl,
                          .nr_workloads = 1,
                          .domains = &domain,
                          .nr_domains = 1,
                          .cooling_alpha = 1};
    init_pac_metadata_pool(&ctx);
    update_pac_entry(&ctx, 0, 4096, score, 1);
    assert(ring_buffer_migration_entry_size(domain.migration_ring) == (size_t)expected);
    khint_t k = pac_table_get(domain.table, pac_key(0, 4096));
    assert(k != kh_end(domain.table));
    assert(kh_val(domain.table, k)->pac_value == score);
    ring_buffer_migration_entry_destroy(domain.migration_ring);
    pac_table_destroy(domain.table);
    pool_destroy(ctx.pac_metadata_pool);
}

#ifdef PACT_DIAG
/* PACT mode with --no-promote-wl 0: a top-bin page of workload 0 stays out
 * of the global queue, and a top-bin page of workload 1 enters it. */
static void check_no_promote_wl(void)
{
    double samples[100] = {0};
    reservoir_t reservoir = {.samples = samples, .capacity = 100};
    binning_state_t bin = {.bin_count = 20, .bin_width = 1000};
    pac_domain_t domain = {.reservoir = &reservoir, .binning = &bin};
    domain.table = pac_table_init();
    domain.migration_ring = ring_buffer_migration_entry_create(8);
    assert(domain.migration_ring);
    pact_workload_t wl[2] = {{.target_pid = 123, .index = 0}, {.target_pid = 124, .index = 1}};
    pact_context_t ctx = {.workloads = wl,
                          .nr_workloads = 2,
                          .domains = &domain,
                          .nr_domains = 1,
                          .cooling_alpha = 1,
                          .no_promote_mask = 1u << 0};
    init_pac_metadata_pool(&ctx);
    update_pac_entry(&ctx, 0, 4096, 19000, 1);
    assert(ring_buffer_migration_entry_size(domain.migration_ring) == 0);
    assert(atomic_load(&wl[0].stats.promotion_attempts) == 0);
    update_pac_entry(&ctx, 1, 4096, 19000, 1);
    assert(ring_buffer_migration_entry_size(domain.migration_ring) == 1);
    migration_entry_t entry;
    assert(ring_buffer_migration_entry_pop(domain.migration_ring, &entry));
    assert(entry.meta->workload == 1 && entry.meta->pid == 124);
    entry.meta->migrating = false;
    ring_buffer_migration_entry_destroy(domain.migration_ring);
    pac_table_destroy(domain.table);
    pool_destroy(ctx.pac_metadata_pool);
}
#endif

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
#ifdef PACT_DIAG
    check_no_promote_wl();
    puts("PACT --no-promote-wl: the masked workload stays out of the queue");
#endif
    /* Finite positive CLI widths can produce an unrepresentable bin index. */
    check_admission(1, 1e-300, true);
    check_admission(1, DBL_MIN / 2, true);
    check_admission(UINT64_MAX, 1, true);
    check_admission(UINT64_MAX, 0.5, true);
    check_admission(0, DBL_MIN / 2, false);
    check_admission(UINT64_MAX, 1e300, false);
    /* Preserve normal threshold behavior on both sides of the top bin. */
    check_admission(18999, 1000, false);
    check_admission(19000, 1000, true);
    check_admission(19001, 1000, true);
    check_admission(9, 0.5, false);
    check_admission(10, 0.5, true);
    puts("bin index: extreme widths and ordinary promotion boundaries passed");
    return 0;
}
