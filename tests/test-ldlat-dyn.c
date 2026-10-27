/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* LDLAT-Dyn governor with temporary cgroup directories: each domain's own
 * cap, per-domain smoothing before the max, the first measurement as the
 * start of each EWMA, the rate after an unreadable tick, and the decision
 * law. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/error.h"
#include "../src/ldlat-dyn.h"
#include "../src/pact.h"

static char dirs[2][64];

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    assert(fp);
    fputs(text, fp);
    fclose(fp);
}

static void set_steals(int d, unsigned long kswapd, unsigned long direct)
{
    char text[128];
    snprintf(text, sizeof(text), "anon 1\npgsteal_kswapd %lu\npgsteal_direct %lu\npgdemote 0\n",
             kswapd, direct);
    write_file(dirs[d], "memory.stat", text);
}

static void remove_dir(int d)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/memory.stat", dirs[d]);
    unlink(path);
    snprintf(path, sizeof(path), "%s/memory.max_at_node0", dirs[d]);
    unlink(path);
    rmdir(dirs[d]);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    pact_workload_t workloads[2];
    memset(workloads, 0, sizeof(workloads));
    for (int d = 0; d < 2; d++) {
        snprintf(dirs[d], sizeof(dirs[d]), "/tmp/pact-ldlat-dyn-XXXXXX");
        assert(mkdtemp(dirs[d]));
        snprintf(workloads[d].cgroup_path, sizeof(workloads[d].cgroup_path), "%s", dirs[d]);
        set_steals(d, 0, 0);
    }
    /* 1000 and 4000 fast-tier pages. */
    write_file(dirs[0], "memory.max_at_node0", "4096000\n");
    write_file(dirs[1], "memory.max_at_node0", "16384000\n");
    pact_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.workloads = workloads;
    ctx.nr_workloads = 2;
    ldlat_dyn_state_t *st = &ctx.ldlat_dyn;

    /* Off: no governor. */
    ldlat_dyn_default_config(&st->cfg);
    assert(ldlat_dyn_init(&ctx) == 1 && !st->enabled);

    ldlat_dyn_default_config(&st->cfg);
    st->cfg.enabled = true;
    st->cfg.start_open = true;
    st->cfg.decide_s = 600; /* no decision in the first part */
    assert(ldlat_dyn_init(&ctx) == 0 && st->enabled);
    assert(st->tier_pages[0] == 1000.0 && st->tier_pages[1] == 4000.0);
    assert(st->mode == LDLAT_DYN_OPEN && st->gate == 0);
    double t0 = st->t0;

    /* Tick 1 reads the baselines only. */
    ldlat_dyn_tick_at(&ctx, t0 + 1);
    assert(!st->have_ema[0] && !st->have_ema[1] && st->pressure == 0.0);

    /* Tick 2: domain 0 evicts 10 pages/s of 1000 (0.01), domain 1 nothing.
     * Each EWMA starts at its first measurement, not at 0. */
    set_steals(0, 6, 4);
    ldlat_dyn_tick_at(&ctx, t0 + 2);
    assert(fabs(st->ema[0] - 0.01) < 1e-12 && st->ema[1] == 0.0);
    assert(fabs(st->pressure - 0.01) < 1e-12 && st->mode == LDLAT_DYN_OPEN);

    /* Tick 3: domain 0 quiet, domain 1 evicts 160 pages/s of 4000 (0.04).
     * Per-domain smoothing then max: max(0.8 * 0.01, 0.2 * 0.04) = 0.008.
     * (Max first then one EWMA would give 0.2 * 0.04 + 0.8 * 0.01 = 0.016,
     * above the emergency level.) */
    set_steals(1, 160, 0);
    ldlat_dyn_tick_at(&ctx, t0 + 3);
    assert(fabs(st->ema[0] - 0.008) < 1e-12 && fabs(st->ema[1] - 0.008) < 1e-12);
    assert(fabs(st->pressure - 0.008) < 1e-12);
    assert(st->mode == LDLAT_DYN_OPEN);

    /* Emergency: domain 1 evicts 400 pages/s (0.1); its EWMA rises to
     * 0.2 * 0.1 + 0.8 * 0.008 = 0.0264 > 0.015 -> TIGHT at once. */
    set_steals(1, 560, 0);
    ldlat_dyn_tick_at(&ctx, t0 + 4);
    assert(st->pressure > 0.015 && st->mode == LDLAT_DYN_TIGHT && st->gate == 700);

    /* Admission: TIGHT gates below 700 cycles. */
    assert(!ldlat_dyn_admit(st, 699) && ldlat_dyn_admit(st, 700));

    /* An unreadable counter skips the domain for the tick and keeps its
     * baseline and EWMA. */
    char stat_path[128];
    snprintf(stat_path, sizeof(stat_path), "%s/memory.stat", dirs[1]);
    unlink(stat_path);
    double ema1 = st->ema[1];
    ldlat_dyn_tick_at(&ctx, t0 + 5);
    assert(st->ema[1] == ema1 && st->last_steals[1] == 560);

    /* The next good read divides by the time since the last good read:
     * 80 pages in 2 s of 4000 pages is 0.01, not 0.02. */
    set_steals(1, 640, 0);
    ldlat_dyn_tick_at(&ctx, t0 + 6);
    assert(fabs(st->ema[1] - (0.2 * 0.01 + 0.8 * ema1)) < 1e-12);

    /* Decisions every 2 s from here. Quiet domains decay the pressure below
     * lo; with enough promotions the gate reopens. */
    st->cfg.decide_s = 2;
    st->cfg.promo_min = 10;
    double t = 6;
    while (st->pressure > st->cfg.lo) {
        t += 1;
        ldlat_dyn_tick_at(&ctx, t0 + t);
    }
    assert(st->mode == LDLAT_DYN_TIGHT); /* no promotions yet: hold */
    workloads[0].stats.promotion_successes += 100;
    t += 2;
    ldlat_dyn_tick_at(&ctx, t0 + t);
    assert(st->mode == LDLAT_DYN_OPEN && st->gate == 0);

    ldlat_dyn_cleanup(&ctx);
    assert(!st->enabled && st->gate == 0);

    /* No domain with a cap of one page or more: the governor cannot engage. */
    write_file(dirs[0], "memory.max_at_node0", "0\n");
    write_file(dirs[1], "memory.max_at_node0", "4095\n");
    ldlat_dyn_default_config(&st->cfg);
    st->cfg.enabled = true;
    assert(ldlat_dyn_init(&ctx) < 0 && !st->enabled);

    remove_dir(0);
    remove_dir(1);
    return 0;
}
