/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* ldlat-dyn.h: LDLAT-Dyn, a self-tuning software latency gate (Tierce,
 * --use-ldlat --ldlat-dyn).
 *
 * The LDLAT event keeps its hardware floor (350 cycles). LDLAT-Dyn switches
 * a software gate over the kept samples between OPEN (no gate) and TIGHT
 * (--ldlat-dyn-tight, 700 cycles) as fast-tier eviction pressure changes.
 * It replaces the static --ldlat-gate: with --ldlat-dyn that gate stays at
 * the 350-cycle floor (its default there), and --ldlat-gate 700 is an error.
 *
 * Signal, every second: for each domain w, E_w = (pages reclaimed from the
 * workload cgroup per second) / (the cgroup's fast-tier pages). Reclaimed
 * pages are pgsteal_kswapd + pgsteal_direct (memory.stat), summed over all
 * nodes. The fast-tier size is memory.max_at_node0. Each domain keeps its
 * own EWMA of E_w (weight --ldlat-dyn-alpha), which starts at the domain's
 * first measurement. E is the largest smoothed value.
 *
 * Law:
 *   every --ldlat-dyn-decide seconds: E > --ldlat-dyn-lo     -> TIGHT
 *                                     else promotions/s >= --ldlat-dyn-promo-min -> OPEN
 *                                     else hold
 *   every tick:                       E > --ldlat-dyn-emerg  -> TIGHT
 *
 * The PEBS aggregator applies the gate after the data-source filter and
 * after the sample fed the workload's k_slow input. A gated sample does not
 * count in the workload's sample count.
 */

#ifndef PACT_LDLAT_DYN_H
#define PACT_LDLAT_DYN_H

#include <stdbool.h>
#include <stdint.h>

struct pact_context;

/* Most domains the governor tracks; equal to MAX_WORKLOADS (pact.h). */
#define LDLAT_DYN_MAX_DOMAINS 16

typedef enum {
    LDLAT_DYN_TIGHT = 0,
    LDLAT_DYN_OPEN = 1,
} ldlat_dyn_mode_t;

/* --ldlat-dyn options. */
typedef struct ldlat_dyn_config {
    bool enabled;     /* --ldlat-dyn */
    uint32_t tight;   /* gate in TIGHT mode, cycles */
    double lo;        /* E above this tightens at a decision */
    double emerg;     /* E above this tightens at any tick */
    double alpha;     /* EWMA weight of the newest E_w */
    double decide_s;  /* decision period, seconds */
    double promo_min; /* promotions/s needed to reopen */
    bool start_open;  /* initial mode OPEN instead of TIGHT */
} ldlat_dyn_config_t;

/* Governor state; one per runtime, held in the context. */
typedef struct ldlat_dyn_state {
    bool enabled;
    ldlat_dyn_config_t cfg;
    ldlat_dyn_mode_t mode;
    uint32_t gate;     /* gate in force, cycles; 0 = OPEN */
    uint64_t admitted; /* samples admitted by the gate (cumulative) */

    int nr_domains;
    double tier_pages[LDLAT_DYN_MAX_DOMAINS];
    uint64_t last_steals[LDLAT_DYN_MAX_DOMAINS];
    double last_steals_t[LDLAT_DYN_MAX_DOMAINS]; /* clock at the read of last_steals */
    bool have_steals[LDLAT_DYN_MAX_DOMAINS];
    double ema[LDLAT_DYN_MAX_DOMAINS];
    bool have_ema[LDLAT_DYN_MAX_DOMAINS];

    double t0;            /* clock at start, seconds */
    double last_tick_t;   /* clock at the last tick */
    double last_decide_t; /* elapsed seconds at the last decision */
    uint64_t last_promotions;
    bool have_promotions;
    uint64_t last_admitted;
    double pressure; /* E of the last tick */
    uint64_t ticks;
    uint64_t mode_changes;
    uint64_t next_tsc; /* event-loop deadline of the next tick */
} ldlat_dyn_state_t;

/* Default --ldlat-dyn options (governor off). */
void ldlat_dyn_default_config(ldlat_dyn_config_t *cfg);

/* Start the governor from ctx->ldlat_dyn.cfg and the workload cgroups.
 * Returns 0 when the governor runs, 1 when LDLAT-Dyn is off, or -1 when it
 * cannot engage (no domain has a fast-tier cap of at least one page). */
int ldlat_dyn_init(struct pact_context *ctx);

/* One governor step at clock `now` (seconds, CLOCK_MONOTONIC). */
void ldlat_dyn_tick_at(struct pact_context *ctx, double now);

/* One governor step at the current clock; the event loop calls it once
 * per second. */
void ldlat_dyn_tick(struct pact_context *ctx);

/* Open the gate. Safe to call when the governor is off. */
void ldlat_dyn_cleanup(struct pact_context *ctx);

/* Gate decision for one kept sample of the given latency. */
static inline bool ldlat_dyn_admit(ldlat_dyn_state_t *st, uint32_t latency)
{
    if (st->gate != 0 && latency < st->gate) {
        return false;
    }
    st->admitted++;
    return true;
}

#endif /* PACT_LDLAT_DYN_H */
