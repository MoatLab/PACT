/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <stdarg.h>
#include "../src/stats-coro.c"

static char captured[1024];
static unsigned int calls;
static void capture(const char *function, const char *format, ...)
{
    (void)function;
    va_list ap;
    va_start(ap, format);
    vsnprintf(captured, sizeof(captured), format, ap);
    va_end(ap);
    calls++;
}

int main(void)
{
    log_info_fn = capture;
    binning_state_t bin = {.bin_width = 100, .bin_count = 10};
    pact_workload_t wl = {.binning = &bin};
    wl.pac_table = pac_table_init();
    log_one_workload_pac_dist(&wl);
    assert(calls == 0);
    pac_metadata_t entries[4] = {0};
    for (int i = 0; i < 4; i++) {
        entries[i].pac_value = UINT64_MAX - (i % 2) * 2;
        entries[i].tier = i / 2;
        int absent;
        khint_t k = pac_table_put(wl.pac_table, (i + 1) * 4096, &absent);
        assert(absent == 1);
        kh_val(wl.pac_table, k) = &entries[i];
    }
    log_one_workload_pac_dist(&wl);
    unsigned int n, above, fast_n, slow_n;
    uint64_t minimum, average, maximum, fast_average, slow_average;
    double threshold;
    assert(sscanf(captured,
                  " WL PAC_DIST: n=%u min=%lu avg=%lu max=%lu threshold=%lf above_thresh=%u "
                  "fast(n=%u avg=%lu) slow(n=%u avg=%lu)",
                  &n, &minimum, &average, &maximum, &threshold, &above, &fast_n, &fast_average,
                  &slow_n, &slow_average) == 10);
    assert(n == 4 && fast_n == 2 && slow_n == 2);
    assert(minimum == UINT64_MAX - 2 && maximum == UINT64_MAX);
    assert(average == UINT64_MAX - 1);
    assert(fast_average == average && slow_average == average);
    assert(threshold == 900 && above == 4);
    for (int tier = 0; tier < 2; tier++) {
        for (int i = 0; i < 4; i++) {
            entries[i].pac_value = 100 + i * 100;
            entries[i].tier = tier;
        }
        log_one_workload_pac_dist(&wl);
        assert(sscanf(captured,
                      " WL PAC_DIST: n=%u min=%lu avg=%lu max=%lu threshold=%lf above_thresh=%u "
                      "fast(n=%u avg=%lu) slow(n=%u avg=%lu)",
                      &n, &minimum, &average, &maximum, &threshold, &above, &fast_n, &fast_average,
                      &slow_n, &slow_average) == 10);
        assert(average == 250 && minimum == 100 && maximum == 400 && above == 0);
        assert(fast_n == (tier == 0 ? 4 : 0) && slow_n == (tier == 1 ? 4 : 0));
        assert(fast_average == (tier == 0 ? 250 : 0));
        assert(slow_average == (tier == 1 ? 250 : 0));
    }
    pac_table_destroy(wl.pac_table);
}
