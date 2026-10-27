/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* Start of the LLC way controller after the per-CPU counting groups
 * opened. A start failure that does not stop the run closes the
 * controller's events in the foreground CPUs' groups, so they free their
 * general-purpose counters. A start that works keeps them. A way bound
 * that does not fit, or a CLOS group that exists already, stops the run. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

static int cat_result;
static int closed[2];

int cat_init(pact_context_t *ctx)
{
    (void)ctx;
    return cat_result;
}

void close_cpu_extra_events(per_cpu_state_t *cs)
{
    closed[cs->owner]++;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    per_cpu_state_t cpus[3];
    for (int c = 0; c < 3; c++) {
        memset(&cpus[c], 0, sizeof(cpus[c]));
        cpus[c].owner = (c < 2) ? CAT_FG_WORKLOAD : CAT_FG_WORKLOAD + 1;
    }
    pact_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mode = PACT_MODE_TIERCE;
    ctx.cpu_states = cpus;
    ctx.nr_all_cpus = 3;
    ctx.cat.cfg.interval_ms = 100;

    /* Without --use-cat nothing starts and nothing closes. */
    cat_result = -1;
    assert(start_cat(&ctx) == 0 && closed[0] == 0);

    /* A start that works keeps the events. */
    ctx.cat.cfg.enabled = true;
    cat_result = 0;
    assert(start_cat(&ctx) == 0 && closed[0] == 0 && closed[1] == 0);

    /* Any other failure: the run continues, and the events close on the
     * two foreground CPUs only. */
    cat_result = -1;
    assert(start_cat(&ctx) == 0 && closed[0] == 2 && closed[1] == 0);

    /* A way bound that does not fit, or a CLOS group that exists already:
     * the run stops (the teardown closes every group). */
    cat_result = -2;
    assert(start_cat(&ctx) < 0 && closed[0] == 2 && closed[1] == 0);
    return 0;
}
