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
            pact_workload_t workload = {0};
            workload.pac_table = pac_table_init();
            assert(workload.pac_table);
            pact_context_t ctx = {.workload = &workload, .max_pac_entries = cap};
            fail_pool = fallback;
            init_pac_metadata_pool(&ctx);
            assert((ctx.pac_metadata_pool == NULL) == fallback);
            for (size_t i = 1; i <= 3; i++) {
                pac_metadata_t *meta = create_pac_entry(&ctx, workload.pac_table, i * 4096, 1, 123);
                assert((meta != NULL) == (cap == 0 || i <= cap));
            }
            assert(kh_size(workload.pac_table) == (cap ? cap : 3));
            assert(workload.stats.pool_alloc_skipped == (cap ? 3 - cap : 0));
            for (khint_t i = 0; i != kh_end(workload.pac_table); i++) {
                if (kh_exist(workload.pac_table, i)) {
                    free_pac_metadata(&ctx, kh_val(workload.pac_table, i));
                }
            }
            pac_table_destroy(workload.pac_table);
            if (ctx.pac_metadata_pool) {
                assert(pool_available(ctx.pac_metadata_pool) ==
                       get_total_capacity(ctx.pac_metadata_pool));
                pool_destroy(ctx.pac_metadata_pool);
            }
        }
    }
    puts("PASS: pooled and heap fallback honor zero, one and two entry limits");
}
