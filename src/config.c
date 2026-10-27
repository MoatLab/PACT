/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* config.c: pact_config_t defaults initialization.
 *
 * Companion: config.h holds the type + PACT_DEFAULT_* macros.
 */

#include <stdbool.h>
#include <string.h>

#include "config.h"

void pact_init_config(pact_config_t *config)
{
    config->mode = PACT_MODE_PACT;
    config->sampling_interval_ms = PACT_DEFAULT_SAMPLING_INTERVAL_MS;
    config->cooling_interval_ms = PACT_DEFAULT_COOLING_INTERVAL_MS;
    config->adaptive_interval_ms = PACT_DEFAULT_ADAPTIVE_INTERVAL_MS;
    config->stats_interval_ms = PACT_DEFAULT_STATS_INTERVAL_MS;
    config->max_migrations_per_cycle = PACT_DEFAULT_MAX_MIGRATIONS_PER_CYCLE;
    config->domain_migration_batch = PACT_DEFAULT_DOMAIN_MIGRATION_BATCH;
    config->enable_logging = false;
    config->log_file[0] = '\0';
    strncpy(config->log_format, "csv", sizeof(config->log_format) - 1);
    config->log_format[sizeof(config->log_format) - 1] = '\0';
    config->pebs_period = 0; /* resolved to the mode's default after parsing */
    config->use_ldlat = false;
    config->ldlat_gate = 0; /* resolved after parsing: pact_config_ldlat_gate() */
    config->ldlat_period = 0;
    ldlat_dyn_default_config(&config->ldlat_dyn);
    config->ldlat_dyn_opt_seen = false;
    cat_default_config(&config->cat);
    config->cat_opt_seen = false;
    config->trace_migration[0] = '\0';
#ifdef PACT_DIAG
    config->dump_pac_table[0] = '\0';
    config->dump_pac_interval_s = 0;
    config->no_promote_mask = 0;
#endif
    config->bin_width = PACT_INITIAL_BIN_WIDTH;
    config->bin_count = PACT_INITIAL_BIN_COUNT;
    config->cooling_alpha = 1.0; /* 1.0 = no cooling (default) */
    config->cooling_trigger_samples = 200000;

    /* At least one --workload is required; CLI parsing or
     * pact_validate_configuration errors out before init. */
    memset(config->workloads, 0, sizeof(config->workloads));
    config->nr_workloads = 0;

    /* CPU affinity (disabled by default) */
    config->monitor_cpu = -1;
    config->migration_cpu = -1;

    config->pac_pool_max = PACT_DEFAULT_PAC_POOL_MAX;
}

uint64_t pact_config_pebs_period(const pact_config_t *config)
{
    if (config->use_ldlat) {
        return config->ldlat_period != 0 ? config->ldlat_period : TIERCE_LDLAT_SAMPLE_PERIOD;
    }
    if (config->pebs_period != 0) {
        return config->pebs_period;
    }
    return config->mode == PACT_MODE_TIERCE ? TIERCE_PEBS_SAMPLE_PERIOD : PACT_PEBS_SAMPLE_PERIOD;
}

uint32_t pact_config_ldlat_gate(const pact_config_t *config)
{
    if (!config->use_ldlat) {
        return 0;
    }
    if (config->ldlat_gate != 0) {
        return config->ldlat_gate;
    }
    return config->ldlat_dyn.enabled ? LDLAT_HW_THRESHOLD : TIERCE_LDLAT_GATE;
}
