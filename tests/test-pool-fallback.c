/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

static bool fail_pool;
object_pool *__real_pool_create(size_t, size_t, size_t, bool);
object_pool *__wrap_pool_create(size_t size, size_t initial, size_t chunk, bool huge)
{
    if (fail_pool) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_pool_create(size, initial, chunk, huge);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    for (unsigned fallback = 0; fallback < 2; fallback++) {
        for (size_t cap = 0; cap <= 2; cap++) {
            pact_workload_t workload = {.target_pid = 123};
            pac_domain_t domain = {0};
            domain.table = pac_table_init();
            assert(domain.table);
            pact_context_t ctx = {
                .workloads = &workload, .nr_workloads = 1, .max_pac_entries = cap};
            fail_pool = fallback;
            init_pac_metadata_pool(&ctx);
            assert((ctx.pac_metadata_pool == NULL) == fallback);
            for (size_t i = 1; i <= 3; i++) {
                pac_metadata_t *meta = create_pac_entry(&ctx, &domain, &workload, i * 4096, 1);
                assert((meta != NULL) == (cap == 0 || i <= cap));
            }
            assert(kh_size(domain.table) == (cap ? cap : 3));
            assert(ctx.stats.pool_alloc_skipped == (cap ? 3 - cap : 0));
            for (khint_t i = 0; i != kh_end(domain.table); i++) {
                if (kh_exist(domain.table, i)) {
                    free_pac_metadata(&ctx, &domain, kh_val(domain.table, i));
                }
            }
            assert(domain.nr_metadata == 0);
            pac_table_destroy(domain.table);
            if (ctx.pac_metadata_pool) {
                assert(pool_available(ctx.pac_metadata_pool) ==
                       get_total_capacity(ctx.pac_metadata_pool));
                pool_destroy(ctx.pac_metadata_pool);
            }
        }
    }
    puts("PASS: pool-backed and heap fallback honor zero, one and two entry limits");
}
