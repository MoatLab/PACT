/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* cli.c — argv parsing for PACT runtime. See `pact --help` for the flag list. */

#include <errno.h>
#include <math.h> /* isfinite */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "error.h" /* set_log_level */
#include "usage.h"
#include "sighandler.h"

int pact_parse_command_line_args(int argc, char *argv[], pact_config_t *config)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sampling-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --sampling-interval requires an argument\n");
                return -1;
            }
            config->sampling_interval_ms = atoi(argv[i]);
        } else if (strcmp(argv[i], "--adaptive-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --adaptive-interval requires an argument\n");
                return -1;
            }
            config->adaptive_interval_ms = atoi(argv[i]);
        } else if (strcmp(argv[i], "--stats-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --stats-interval requires an argument\n");
                return -1;
            }
            config->stats_interval_ms = atoi(argv[i]);
        } else if (strcmp(argv[i], "--max-migrations-per-cycle") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --max-migrations-per-cycle requires an argument\n");
                return -1;
            }
            int max_migrations = atoi(argv[i]);
            if (max_migrations < 1) {
                fprintf(stderr, "Error: --max-migrations-per-cycle must be >= 1\n");
                return -1;
            }
            config->max_migrations_per_cycle = (uint32_t)max_migrations;
        } else if (strcmp(argv[i], "--demotion-margin") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --demotion-margin requires an argument\n");
                return -1;
            }
            /* strtoull silently wraps negatives ("-1" -> UINT64_MAX) and
             * accepts trailing garbage; require a plain non-negative
             * integer that round-trips. */
            errno = 0;
            char *end = NULL;
            unsigned long long margin = strtoull(argv[i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || argv[i][0] == '-') {
                fprintf(stderr, "Error: --demotion-margin must be a non-negative integer\n");
                return -1;
            }
            config->demotion_margin = (uint64_t)margin;
        } else if (strcmp(argv[i], "--bin-count") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --bin-count requires an argument\n");
                return -1;
            }
            int bin_count = atoi(argv[i]);
            if (bin_count < 1) {
                fprintf(stderr, "Error: --bin-count must be >= 1\n");
                return -1;
            }
            config->bin_count = (uint32_t)bin_count;
        } else if (strcmp(argv[i], "--bin-width") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --bin-width requires an argument\n");
                return -1;
            }
            config->bin_width = atof(argv[i]);
            if (!(config->bin_width > 0.0) || !isfinite(config->bin_width)) {
                fprintf(stderr, "Error: --bin-width must be a finite value > 0\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--cooling-alpha") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cooling-alpha requires an argument\n");
                return -1;
            }
            config->cooling_alpha = atof(argv[i]);
            /* !(x >= 0 && x <= 1) also rejects NaN, which passes a
             * naive (x < 0 || x > 1) test and corrupts the bin index. */
            if (!(config->cooling_alpha >= 0.0 && config->cooling_alpha <= 1.0)) {
                fprintf(stderr, "Error: --cooling-alpha must be in [0, 1]\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--cooling-trigger-samples") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cooling-trigger-samples requires an argument\n");
                return -1;
            }
            config->cooling_trigger_samples = strtoull(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--logging") == 0) {
            config->enable_logging = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                if (strcmp(argv[i], "csv") == 0 || strcmp(argv[i], "json") == 0) {
                    strncpy(config->log_format, argv[i], sizeof(config->log_format) - 1);
                    config->log_format[sizeof(config->log_format) - 1] = '\0';
                } else {
                    fprintf(stderr, "Warning: Unknown log format '%s', using 'csv'\n", argv[i]);
                }
            }
        } else if (strcmp(argv[i], "--log-file") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --log-file requires an argument\n");
                return -1;
            }
            strncpy(config->log_file, argv[i], sizeof(config->log_file) - 1);
            config->log_file[sizeof(config->log_file) - 1] = '\0';
        } else if (strcmp(argv[i], "--pebs-period") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --pebs-period requires an argument\n");
                return -1;
            }
            config->pebs_period = atoll(argv[i]);
            if (config->pebs_period < 1 || config->pebs_period > 1000000) {
                fprintf(stderr, "Error: PEBS period must be between 1 and 1000000\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--workload") == 0) {
            /* --workload PID is required. CPU pinning is external
             * (taskset). Passing --workload more than once overwrites
             * the prior value with a warning. */
            if (++i >= argc) {
                fprintf(stderr, "Error: --workload requires a PID\n");
                return -1;
            }
            pid_t pid = atoi(argv[i]);
            if (pid <= 0) {
                fprintf(stderr, "Error: Invalid PID: %s\n", argv[i]);
                return -1;
            }
            if (config->target_pid > 0) {
                fprintf(stderr,
                        "Warning: --workload specified more than once; "
                        "overwriting PID %d with %d\n",
                        config->target_pid, pid);
            }
            config->target_pid = pid;
            snprintf(config->workload_name, sizeof(config->workload_name), "workload");
            printf("Workload: PID %d\n", pid);
        } else if (strcmp(argv[i], "--log-level") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --log-level requires an argument\n");
                return -1;
            }
            int log_level = atoi(argv[i]);
            if (log_level < 0 || log_level > 4) {
                fprintf(stderr, "Error: Log level must be between 0 and 4\n");
                return -1;
            }
            set_log_level(log_level);
            printf("Log level set to %d\n", log_level);
        } else if (strcmp(argv[i], "--monitor-cpu") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --monitor-cpu requires an argument\n");
                return -1;
            }
            config->monitor_cpu = atoi(argv[i]);
            printf("Monitor CPU affinity set to %d\n", config->monitor_cpu);
        } else if (strcmp(argv[i], "--migration-cpu") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --migration-cpu requires an argument\n");
                return -1;
            }
            config->migration_cpu = atoi(argv[i]);
            printf("Migration CPU affinity set to %d\n", config->migration_cpu);
        } else if (strcmp(argv[i], "--crash-marker") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --crash-marker requires a path argument\n");
                return -1;
            }
            pact_signal_set_crash_marker_path(argv[i]);
            printf("Crash marker path: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--pac-pool-max") == 0) {
            if (++i >= argc) {
                fprintf(stderr,
                        "Error: --pac-pool-max requires an argument (entries; 0=unlimited)\n");
                return -1;
            }
            config->pac_pool_max = (size_t)strtoull(argv[i], NULL, 10);
            printf("PAC metadata pool cap: %zu entries (~%zu MB at 128B/entry)\n",
                   config->pac_pool_max, config->pac_pool_max * 128 / (1024 * 1024));
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            pact_print_usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            pact_print_version();
            return 1;
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            pact_print_usage(argv[0]);
            return -1;
        }
    }

    if (config->target_pid <= 0) {
        pact_print_usage(argv[0]);
        fprintf(stderr, "\nError: --workload PID is required.\n");
        return -1;
    }
    return 0;
}
