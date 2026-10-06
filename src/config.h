/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* config.h — pact_config_t (CLI/init time configuration).
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

#include "pact.h" /* demotion_policy_t */

/* Default config values exposed for validation + init. */
#define PACT_PEBS_SAMPLE_PERIOD 400U
#define PACT_DEFAULT_SAMPLING_INTERVAL_MS 20U
#define PACT_DEFAULT_COOLING_INTERVAL_MS 5000U
#define PACT_DEFAULT_ADAPTIVE_INTERVAL_MS 20U
#define PACT_DEFAULT_STATS_INTERVAL_MS 5000U
/* Cap on pages promoted per 20 ms cycle. A low cap throttles promotion so
 * PACT converges too slowly to relieve slow-tier pressure; 4096 keeps the
 * critical set moving without overwhelming the migration thread. Override
 * with --max-migrations-per-cycle. */
#define PACT_DEFAULT_MAX_MIGRATIONS_PER_CYCLE 4096U
#define PACT_INITIAL_BIN_WIDTH 1000.0
#define PACT_INITIAL_BIN_COUNT 20U
#define PACT_DEFAULT_PAC_POOL_MAX (2UL * 1024 * 1024) /* 2M tracked pages */

typedef struct pact_config {
    demotion_policy_t demotion_policy;
    uint32_t sampling_interval_ms;
    uint32_t cooling_interval_ms;
    uint32_t adaptive_interval_ms;
    uint32_t stats_interval_ms;
    uint32_t max_migrations_per_cycle;
    uint64_t demotion_margin; /* Algorithm 2 aggressiveness m (default 0) */
    uint64_t pebs_period;
    uint32_t bin_count;
    double bin_width;
    double cooling_alpha;             /* α ∈ [0, 1]; 1.0 = disabled (default) */
    uint64_t cooling_trigger_samples; /* trigger threshold (default 200K) */
    bool enable_logging;
    char log_format[16];
    char log_file[256];

    /* Workload identification. target_pid is required (CLI errors out if
     * absent); sentinel value <= 0 in the config struct only at parse
     * time. */
    pid_t target_pid;
    char workload_name[64];

    /* CPU affinity */
    int monitor_cpu;
    int migration_cpu;

    /* PAC metadata pool cap */
    size_t pac_pool_max;
} pact_config_t;

/* Initialize all fields of `config` to documented defaults. */
void pact_init_config(pact_config_t *config);

#endif /* PACT_CONFIG_H */
