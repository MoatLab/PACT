/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main
#undef MINICORO_IMPL
#include "../src/stats-coro.c"

static char captured[1024];

static void capture(const char *function, const char *format, ...)
{
    (void)function;
    va_list ap;
    va_start(ap, format);
    vsnprintf(captured, sizeof(captured), format, ap);
    va_end(ap);
}

static void check(uint64_t score, double width, size_t bins, bool expected)
{
    double samples[100] = {0};
    reservoir_t reservoir = {.samples = samples, .capacity = 100};
    binning_state_t bin = {.bin_count = bins, .bin_width = width};
    pact_workload_t wl = {.reservoir = &reservoir, .binning = &bin};
    wl.pac_table = pac_table_init();
    wl.migration_ring = ring_buffer_migration_entry_create(8);
    assert(wl.migration_ring);
    pact_context_t ctx = {.workload = &wl, .cooling_alpha = 1};
    init_pac_metadata_pool(&ctx);
    update_pac_entry(&ctx, 4096, score, 1, 123);
    size_t admitted = ring_buffer_migration_entry_size(wl.migration_ring);
    assert(admitted == (size_t)expected);
    log_one_workload_pac_dist(&wl);
    double threshold;
    unsigned above;
    char *field = strstr(captured, "threshold=");
    assert(field && sscanf(field, "threshold=%lf above_thresh=%u", &threshold, &above) == 2);
    assert(above == admitted);
    assert(threshold == width * (bins - 1));
    ring_buffer_migration_entry_destroy(wl.migration_ring);
    pac_table_destroy(wl.pac_table);
    pool_destroy(ctx.pac_metadata_pool);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    log_info_fn = capture;
    check(1, .1, 20, false);
    check(2, .1, 20, true);
    check(9, .5, 20, false);
    check(10, .5, 20, true);
    check(18999, 1000, 20, false);
    check(19000, 1000, 20, true);
    check(UINT64_MAX, 1e300, 20, false);
    check(UINT64_MAX, DBL_MAX, 20, false);
    check(0, DBL_MAX, 1, true);
    puts("PAC threshold diagnostics match actual promotion admission: 9 cases "
         "passed");
    return 0;
}
