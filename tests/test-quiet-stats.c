/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <stdarg.h>
#define MINICORO_IMPL
#include "../src/minicoro.h"
#undef MINICORO_IMPL
#include "../src/stats-coro.c"

static unsigned calls;
static bool distribution_seen;

static void capture(const char *function, const char *format, ...)
{
    (void)function;
    calls++;
    if (strstr(format, "PAC_DIST")) {
        distribution_seen = true;
    }
}

static void check_level(int level)
{
    binning_state_t bin = {.bin_width = 1000, .bin_count = 20};
    pac_metadata_t entry = {.pac_value = 19000, .tier = 1};
    pact_workload_t workload = {.binning = &bin, .name = "quiet-stats-test"};
    workload.pac_table = pac_table_init();
    int absent;
    khint_t key = pac_table_put(workload.pac_table, 4096, &absent);
    assert(absent);
    kh_val(workload.pac_table, key) = &entry;
    pact_context_t context = {.workload = &workload,
                              .running = true,
                              .tsc_freq_hz = 1000000000,
                              .start_tsc = rdtsc() - 1000000};
    set_log_level(level);
    /* Observe calls normally discarded by the logger at quiet levels. */
    log_info_fn = capture;
    calls = 0;
    distribution_seen = false;
    mco_desc desc = mco_desc_init(stats_coroutine, 0);
    desc.user_data = &context;
    mco_coro *co = NULL;
    assert(mco_create(&co, &desc) == MCO_SUCCESS);
    assert(mco_resume(co) == MCO_SUCCESS);
    assert(mco_status(co) == MCO_SUSPENDED);
    if (level < LOG_LEVEL_INFO) {
        assert(calls == 0 && !distribution_seen);
    } else {
        assert(calls > 0 && distribution_seen);
    }
    context.running = false;
    assert(mco_resume(co) == MCO_SUCCESS);
    assert(mco_status(co) == MCO_DEAD);
    assert(mco_destroy(co) == MCO_SUCCESS);
    pac_table_destroy(workload.pac_table);
}

int main(void)
{
    for (int level = LOG_LEVEL_ERROR; level <= LOG_LEVEL_TRACE; level++) {
        check_level(level);
    }
    puts("Statistics coroutine: quiet levels skip collection; enabled levels report and all levels "
         "shut down");
}
