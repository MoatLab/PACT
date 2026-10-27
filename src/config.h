/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* config.h: pact_config_t (CLI/init time configuration).
 *
 * pact_config_t holds startup-time configuration. It is populated by
 * pact_parse_command_line_args + pact_init_config defaults, then handed to
 * initialize_pact_context which copies relevant fields into pact_context_t.
 *
 * Naming: pact_config_t is the "request"; pact_context_t is the "state".
 */

#ifndef PACT_CONFIG_H
#define PACT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pact.h" /* pact_mode_t, MAX_WORKLOADS */

/* Default config values exposed for validation + init. */
#define PACT_PEBS_SAMPLE_PERIOD 400U   /* --pebs-period default in PACT mode */
#define TIERCE_PEBS_SAMPLE_PERIOD 100U /* --pebs-period default in Tierce mode */
#define TIERCE_LDLAT_SAMPLE_PERIOD 5U  /* --ldlat-period default (Tierce, --use-ldlat) */
#define TIERCE_LDLAT_GATE 700U         /* --ldlat-gate default without --ldlat-dyn (paper) */
#define PACT_DEFAULT_SAMPLING_INTERVAL_MS 20U
#define PACT_DEFAULT_COOLING_INTERVAL_MS 5000U
#define PACT_DEFAULT_ADAPTIVE_INTERVAL_MS 20U
#define PACT_DEFAULT_STATS_INTERVAL_MS 5000U
/* Largest --sampling-interval, --adaptive-interval and --stats-interval
 * (one hour); the smallest is 1 ms. */
#define PACT_MAX_INTERVAL_MS 3600000UL
/* Most pages per numa_move_pages() call. The migration worker drains the
 * whole queue on each wake, in calls of at most this many pages. Override
 * with --max-migrations-per-cycle. */
#define PACT_DEFAULT_MAX_MIGRATIONS_PER_CYCLE 4096U
/* Tierce: most pages one domain moves per round-robin turn, in one
 * numa_move_pages() call. Override with --domain-migration-batch. */
#define PACT_DEFAULT_DOMAIN_MIGRATION_BATCH 32U
#define PACT_INITIAL_BIN_WIDTH 1000.0
#define PACT_INITIAL_BIN_COUNT 20U
#define PACT_DEFAULT_PAC_POOL_MAX 0UL /* tracked pages per PAC table; 0 = unlimited */

/* The CPU list word that keeps the process affinity as the CPU set. */
#define PACT_CPUS_AUTO "auto"

/* One --workload PID [CPUS [NAME [CGROUP]]] argument. */
typedef struct pact_workload_config {
    pid_t pid;
    char cpus[256];   /* CPU list such as 2-5,8; "" or "auto" = process affinity */
    char name[64];    /* "" = workload<N> */
    char cgroup[256]; /* cgroup directory: absolute, or relative to /sys/fs/cgroup */
} pact_workload_config_t;

typedef struct pact_config {
    pact_mode_t mode;
    uint32_t sampling_interval_ms;
    uint32_t cooling_interval_ms;
    uint32_t adaptive_interval_ms;
    uint32_t stats_interval_ms;
    uint32_t max_migrations_per_cycle;
    uint32_t domain_migration_batch;
    uint64_t pebs_period; /* 0 until set: the mode's default applies */

    /* Tierce LDLAT: sample loads above the hardware latency floor and
     * weight each slow-tier sample by its latency. */
    bool use_ldlat;
    uint32_t ldlat_gate;   /* 0 until set: see pact_config_ldlat_gate() */
    uint64_t ldlat_period; /* 0 until set: TIERCE_LDLAT_SAMPLE_PERIOD */
    ldlat_dyn_config_t ldlat_dyn;
    bool ldlat_dyn_opt_seen; /* an --ldlat-dyn-* option was given */

    /* Tierce LLC way controller */
    cat_config_t cat;
    bool cat_opt_seen; /* a --cat-* option was given */

    char trace_migration[256]; /* --trace-migration path; "" = off */
#ifdef PACT_DIAG
    char dump_pac_table[256];     /* --dump-pac-table path; "" = off */
    uint32_t dump_pac_interval_s; /* 0 = PAC_DUMP_DEFAULT_INTERVAL_S */
    uint32_t no_promote_mask;
#endif
    uint32_t bin_count;
    double bin_width;
    double cooling_alpha;             /* α ∈ [0, 1]; 1.0 = disabled (default) */
    uint64_t cooling_trigger_samples; /* trigger threshold (default 200K) */
    bool enable_logging;
    char log_format[16];
    char log_file[256];

    /* Workloads in --workload order; at least one is required. */
    pact_workload_config_t workloads[MAX_WORKLOADS];
    int nr_workloads;

    /* CPU affinity */
    int monitor_cpu;
    int migration_cpu;

    /* PAC metadata pool cap */
    size_t pac_pool_max;
} pact_config_t;

/* Initialize all fields of `config` to documented defaults. */
void pact_init_config(pact_config_t *config);

/* The sample period of the open PEBS event: with --use-ldlat,
 * --ldlat-period (default 5); else --pebs-period, or the mode's default
 * (400 in PACT mode, 100 in Tierce mode). */
uint64_t pact_config_pebs_period(const pact_config_t *config);

/* The software LDLAT gate in cycles: --ldlat-gate (350 or 700) when given;
 * else 700 (TIERCE_LDLAT_GATE, the paper's gate), or 350 with --ldlat-dyn,
 * where LDLAT-Dyn switches its own gate between open (the 350-cycle floor)
 * and tight (700 by default, the start mode). 350 keeps every sample above
 * the hardware floor. 0 without --use-ldlat. */
uint32_t pact_config_ldlat_gate(const pact_config_t *config);

#endif /* PACT_CONFIG_H */
