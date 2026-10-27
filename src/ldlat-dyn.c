/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* ldlat-dyn.c: the LDLAT-Dyn governor (see ldlat-dyn.h for the law). */

#include <string.h>
#include <time.h>

#include "cgroup.h"
#include "error.h"
#include "ldlat-dyn.h"
#include "pact.h"
#include "tsc.h"

_Static_assert(LDLAT_DYN_MAX_DOMAINS == MAX_WORKLOADS, "one governor slot per workload domain");

void ldlat_dyn_default_config(ldlat_dyn_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tight = 700;
    cfg->lo = 0.002;
    cfg->emerg = 0.015;
    cfg->alpha = 0.2;
    cfg->decide_s = 30.0;
    cfg->promo_min = 200.0;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *mode_name(ldlat_dyn_mode_t mode)
{
    return mode == LDLAT_DYN_TIGHT ? "TIGHT" : "OPEN";
}

static void set_mode(ldlat_dyn_state_t *st, ldlat_dyn_mode_t mode)
{
    st->gate = (mode == LDLAT_DYN_TIGHT) ? st->cfg.tight : 0;
    if (st->mode != mode) {
        st->mode_changes++;
    }
    st->mode = mode;
}

/* Reclaimed pages of a cgroup on all nodes: pgsteal_kswapd + pgsteal_direct. */
static int read_cgroup_steals(const char *cgroup_path, uint64_t *steals)
{
    static const char *const keys[] = {"pgsteal_kswapd", "pgsteal_direct"};
    return pact_cgroup_stat_sum(cgroup_path, keys, 2, steals);
}

int ldlat_dyn_init(pact_context_t *ctx)
{
    ldlat_dyn_state_t *st = &ctx->ldlat_dyn;
    ldlat_dyn_config_t cfg = st->cfg;
    memset(st, 0, sizeof(*st));
    st->cfg = cfg;
    if (!cfg.enabled) {
        return 1;
    }

    /* Each domain's own fast-tier cap, in pages. */
    int usable = 0;
    st->nr_domains =
        ctx->nr_workloads < LDLAT_DYN_MAX_DOMAINS ? ctx->nr_workloads : LDLAT_DYN_MAX_DOMAINS;
    for (int d = 0; d < st->nr_domains; d++) {
        uint64_t bytes = 0;
        if (pact_cgroup_node0_cap(ctx->workloads[d].cgroup_path, &bytes) == 0) {
            st->tier_pages[d] = (double)bytes / (double)PAGE_SIZE;
        }
        if (st->tier_pages[d] >= 1.0) {
            usable++;
        } else {
            log_warning("ldlat_dyn", "Workload %d: fast-tier cap below one page; not tracked", d);
        }
    }
    if (usable == 0) {
        log_warning("ldlat_dyn", "No workload has a fast-tier cap of at least one page");
        return -1;
    }

    st->t0 = monotonic_seconds();
    st->last_tick_t = st->t0;
    st->mode = cfg.start_open ? LDLAT_DYN_OPEN : LDLAT_DYN_TIGHT;
    set_mode(st, st->mode);
    st->mode_changes = 0;
    st->enabled = true;
    log_info("ldlat_dyn",
             "LDLAT-Dyn on: floor=%u tight=%u lo=%.4f emerg=%.4f alpha=%.2f decide=%.0fs "
             "promo_min=%.0f/s domains=%d start=%s",
             LDLAT_HW_THRESHOLD, cfg.tight, cfg.lo, cfg.emerg, cfg.alpha, cfg.decide_s,
             cfg.promo_min, usable, mode_name(st->mode));
    return 0;
}

/* Update each domain's smoothed eviction pressure and return E, the
 * largest smoothed value (0 before the first measurement). *turnover and
 * *falls_rate report the largest raw value of this tick. An unreadable
 * counter skips the domain for this tick and keeps its baseline. The rate
 * divides by the time since the domain's last good read, so the evictions
 * of skipped ticks count over their own time. */
static double update_pressure(pact_context_t *ctx, double now, double *turnover, double *falls_rate)
{
    ldlat_dyn_state_t *st = &ctx->ldlat_dyn;
    double pressure = 0.0;
    *turnover = 0.0;
    *falls_rate = 0.0;
    for (int d = 0; d < st->nr_domains; d++) {
        if (st->tier_pages[d] < 1.0) {
            continue;
        }
        uint64_t steals;
        if (read_cgroup_steals(ctx->workloads[d].cgroup_path, &steals) == 0) {
            if (st->have_steals[d]) {
                uint64_t delta = steals >= st->last_steals[d] ? steals - st->last_steals[d] : 0;
                double rate = (double)delta / (now - st->last_steals_t[d]);
                double turn = rate / st->tier_pages[d];
                st->ema[d] = st->have_ema[d]
                                 ? st->cfg.alpha * turn + (1.0 - st->cfg.alpha) * st->ema[d]
                                 : turn;
                st->have_ema[d] = true;
                if (turn > *turnover) {
                    *turnover = turn;
                    *falls_rate = rate;
                }
            }
            st->last_steals[d] = steals;
            st->last_steals_t[d] = now;
            st->have_steals[d] = true;
        }
        if (st->have_ema[d] && st->ema[d] > pressure) {
            pressure = st->ema[d];
        }
    }
    return pressure;
}

static uint64_t total_promotions(const pact_context_t *ctx)
{
    uint64_t total = 0;
    for (int w = 0; w < ctx->nr_workloads; w++) {
        total += ctx->workloads[w].stats.promotion_successes;
    }
    return total;
}

void ldlat_dyn_tick_at(pact_context_t *ctx, double now)
{
    ldlat_dyn_state_t *st = &ctx->ldlat_dyn;
    if (!st->enabled) {
        return;
    }
    double interval = now - st->last_tick_t;
    double elapsed = now - st->t0;
    if (interval < 0.1) {
        return;
    }
    st->last_tick_t = now;
    st->ticks++;

    double turnover, falls_rate;
    double pressure = update_pressure(ctx, now, &turnover, &falls_rate);
    st->pressure = pressure;

    uint64_t promotions = total_promotions(ctx);
    double promo_rate = 0.0;
    if (st->have_promotions && promotions >= st->last_promotions) {
        promo_rate = (double)(promotions - st->last_promotions) / interval;
    }
    st->last_promotions = promotions;
    st->have_promotions = true;
    double admitted_rate = (double)(st->admitted - st->last_admitted) / interval;
    st->last_admitted = st->admitted;

    ldlat_dyn_mode_t mode = st->mode;
    const char *why = "";
    if (pressure > st->cfg.emerg && mode != LDLAT_DYN_TIGHT) {
        mode = LDLAT_DYN_TIGHT;
        why = "emergency";
    }
    if (elapsed - st->last_decide_t >= st->cfg.decide_s) {
        st->last_decide_t = elapsed;
        if (pressure > st->cfg.lo) {
            mode = LDLAT_DYN_TIGHT;
            why = "pressure";
        } else if (promo_rate >= st->cfg.promo_min) {
            mode = LDLAT_DYN_OPEN;
            why = "quiet+promotions";
        } else {
            why = "no-evidence";
        }
    }
    bool changed = mode != st->mode;
    set_mode(st, mode);

    if (changed || st->ticks % 10 == 0) {
        log_info("ldlat_dyn",
                 "LDLATDYN t=%.1f falls_rate=%.0f/s turnover=%.5f pressure=%.5f promos=%.0f/s "
                 "admitted=%.0f/s gate=%u mode=%s%s%s",
                 elapsed, falls_rate, turnover, pressure, promo_rate, admitted_rate, st->gate,
                 mode_name(st->mode), why[0] ? " why=" : "", why);
    }
}

void ldlat_dyn_tick(pact_context_t *ctx)
{
    ldlat_dyn_tick_at(ctx, monotonic_seconds());
}

void ldlat_dyn_cleanup(pact_context_t *ctx)
{
    ldlat_dyn_state_t *st = &ctx->ldlat_dyn;
    if (!st->enabled) {
        return;
    }
    log_info("ldlat_dyn", "Stopped after %lu ticks, %lu mode changes, final mode %s", st->ticks,
             st->mode_changes, mode_name(st->mode));
    st->gate = 0;
    st->enabled = false;
}
