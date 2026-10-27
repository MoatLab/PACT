/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* cat.h: bounded LLC way control through Intel CAT (resctrl), Tierce mode
 * (--use-cat).
 *
 * A bounded hysteresis controller keeps the foreground (FG) workload's way
 * count W inside [W_min, W_max]: it restores W_max when the demand signal
 * exceeds tau_hi and releases one way when the signal stays below tau_lo.
 * The first --workload is the FG; every later workload is background (BG)
 * and shares the remaining ways.
 *
 * At start the controller mounts /sys/fs/resctrl when needed, reads the L3
 * CBM capabilities, creates two CLOS groups (tierce_fg for the FG's CPUs,
 * tierce_bg for the other workloads' CPUs), programs W = W_max, and then
 * adjusts the split every --cat-interval ms from the event loop.
 *
 * The counter signals read the FG's per-CPU counting groups, which the
 * runtime stops and resets around every sampling window: the controller
 * adds up its own window counts (the feed). A tick in which an FG CPU group
 * read failed is skipped. When the FG groups are multiplexed (running below
 * 99% of enabled over a tick), the controller parks at W_max until a
 * measured tick shows them whole again.
 *
 * Without resctrl, CAT is disabled and the run continues.
 */

#ifndef PACT_CAT_H
#define PACT_CAT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pmu.h" /* CPU_EVENT_COUNT */

struct pact_context;

/* The CAT foreground is the first --workload. */
#define CAT_FG_WORKLOAD 0

/* Maximum L3 (cache) domains tracked for the schemata line (sockets). */
#define CAT_MAX_DOMAINS 8

/* Demand signals. */
typedef enum cat_signal {
    CAT_SIGNAL_STATIC = 0, /* hold W_max */
    CAT_SIGNAL_MPKI,       /* LLC misses per 1000 instructions */
    CAT_SIGNAL_L2LAT,      /* cycles per L2-missing demand load (OFFCORE pair) */
    CAT_SIGNAL_PROBE,      /* trial withdrawal judged on FG IPC */
    CAT_SIGNAL_BW,         /* FG memory bandwidth from resctrl MBM */
    CAT_SIGNAL_COUNT
} cat_signal_t;

typedef enum cat_probe_state {
    CAT_PROBE_WARMUP = 0,
    CAT_PROBE_HOLD,
    CAT_PROBE_PROBING
} cat_probe_state_t;

/* Default release/restore thresholds, in the signal's unit. probe, bw and
 * static do not use them. */
#define CAT_TAU_LO_MPKI 10.0
#define CAT_TAU_HI_MPKI 25.0
#define CAT_TAU_LO_L2LAT 110.0
#define CAT_TAU_HI_L2LAT 140.0

/* General-purpose counters l2lat needs per CPU: the PEBS event, the
 * slow-tier LLC-miss event and the two OFFCORE events. */
#define CAT_L2LAT_GP_COUNTERS 4

#define CAT_PROBE_WIN_MAX 64
#define CAT_BW_SAMPLES_MAX 4096

/* --use-cat and --cat-* options. */
typedef struct cat_config {
    bool enabled;
    cat_signal_t signal;
    int wmin; /* -1 = (ways + 2) / 4 */
    int wmax; /* -1 = 3 * ways / 4 */
    uint32_t interval_ms;
    double tau_lo; /* < 0 until resolved: the signal's default */
    double tau_hi;
    double probe_tol;
    double probe_s;
    double cooldown_s;
    double bw_thresh_gbps; /* 0 = calibrate from the warmup */
    double bw_band_gbps;   /* -1 = automatic */
    double bw_warmup_s;
    char trace_file[256]; /* DIAG build only */
} cat_config_t;

/* The FG's window counts and clocks, summed by the counter reader. */
typedef struct cat_feed {
    bool active;      /* the reader adds FG windows */
    bool read_failed; /* an FG CPU group read failed since the last tick */
    uint64_t count[CPU_EVENT_COUNT];
    bool have[CPU_EVENT_COUNT]; /* event open on every FG CPU */
    uint64_t time_enabled;
    uint64_t time_running;
} cat_feed_t;

typedef struct cat_state {
    bool enabled;
    cat_config_t cfg;
    cat_feed_t feed;
    uint32_t events; /* extra per-CPU events the signal needs (1 << cpu_event_index_t) */
    uint64_t next_tsc;

    bool mounted_by_us;
    bool fg_created; /* this run created the FG group (the cleanup removes it) */
    bool bg_created;
    bool warned_no_signal;
    bool warned_multiplexed;
    bool multiplexed;
    bool warned_low_rate;
    bool signal_dead;
    bool seen_instr;
    int zero_streak;

    int num_ways;
    uint64_t full_mask;
    unsigned int min_cbm_bits;
    unsigned int num_closids;
    bool has_mon;
    int domain_ids[CAT_MAX_DOMAINS];
    int nr_domains;

    cat_signal_t signal;
    int w_min;
    int w_max;
    int w_fg; /* FG ways of the split in the hardware; 0 before the first split */
    double tau_lo;
    double tau_hi;

    char fg_grp[256];
    char bg_grp[256];

    /* Feed values at the previous tick. */
    uint64_t last_instr;
    uint64_t last_cycles;
    uint64_t last_llc_all;
    uint64_t last_off_out;
    uint64_t last_off_dd;
    uint64_t last_time_enabled;
    uint64_t last_time_running;
    bool have_snapshot;

    /* probe */
    double probe_tol;
    int probe_win;
    double probe_s;
    double cooldown_s;
    double ipc_win[CAT_PROBE_WIN_MAX];
    int ipc_n;
    double ref_ipc;
    bool have_ref;
    double ipc_before;
    double next_probe_t;
    cat_probe_state_t probe_state;

    /* bw */
    double bw_thresh_gbps;
    double bw_band_gbps;
    double bw_hyst;
    double bw_warmup_s;
    uint64_t last_mbm_bytes;
    bool have_mbm;
    double bw_samples[CAT_BW_SAMPLES_MAX];
    int bw_nsamples;
    bool bw_calibrated;

    /* trace */
    uint64_t trace_last_instr;
    uint64_t trace_last_mbm_fg;
    uint64_t trace_last_mbm_bg;
    double trace_last_mbm_t;
    bool have_trace_mbm;
    bool trace_have_instr;
    uint64_t fg_instr_total;

    double t0;
    double last_t;
    uint64_t ticks;
    uint64_t reallocations;
    FILE *trace;
} cat_state_t;

/* Default --cat-* options (controller off, mpki, I = 100 ms). */
void cat_default_config(cat_config_t *cfg);

/* An unset tau takes the signal's default (the units differ). */
void cat_resolve_taus(cat_config_t *cfg);

int cat_signal_parse(const char *name, cat_signal_t *out);
const char *cat_signal_name(cat_signal_t sig);

/* General-purpose counters per CPU (CPUID leaf 0xA). */
int cat_gp_counters(void);

/* The extra per-CPU events the signal needs, in *events (with_trace: also
 * the instructions event for the trace). Returns -1 when the signal cannot
 * run on this CPU: l2lat needs more than CAT_L2LAT_GP_COUNTERS
 * general-purpose counters (gp_counters). */
int cat_signal_events(cat_signal_t sig, bool with_trace, int gp_counters, uint32_t *events);

/* Start the controller from ctx->cat.cfg. Returns 0 when it runs, -1 when
 * CAT is disabled (the run may continue), or -2 when the run stops: an
 * explicit way bound does not fit the cache, or a CLOS group of the
 * controller exists already. */
int cat_init(struct pact_context *ctx);

/* One controller tick (every --cat-interval ms). No-op when disabled. */
void cat_control_step(struct pact_context *ctx);

/* Write the full masks, remove the CLOS groups, and unmount resctrl if the
 * controller mounted it. A failed step logs a warning. Safe to call when
 * disabled. */
void cat_cleanup(struct pact_context *ctx);

/* The bounded hysteresis law (mpki and l2lat): restore W_max above tau_hi;
 * release one way in each interval below tau_lo. Returns the action name. */
const char *cat_hysteresis_law(cat_state_t *cat, double sig);

#endif /* PACT_CAT_H */
