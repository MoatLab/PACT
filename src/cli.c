/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* cli.c: argv parsing for PACT runtime. See `pact --help` for the flag list. */

#include <errno.h>
#include <limits.h>
#include <math.h>  /* isfinite */
#include <sched.h> /* CPU_SETSIZE */
#include <stdbool.h>
#include <stddef.h> /* offsetof */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "constants.h" /* MIGRATION_RING_DEFAULT_SIZE */
#include "error.h"     /* set_log_level */
#include "usage.h"
#include "sighandler.h"

/* Options that belong to one mode. Giving one in the other mode is an
 * error, so a flag never has a silent no-op meaning. */
static const struct {
    const char *name;
    pact_mode_t mode;
} mode_options[] = {
    {"--bin-count", PACT_MODE_PACT},
    {"--bin-width", PACT_MODE_PACT},
    {"--max-migrations-per-cycle", PACT_MODE_PACT},
    {"--domain-migration-batch", PACT_MODE_TIERCE},
    {"--use-ldlat", PACT_MODE_TIERCE},
    {"--ldlat-gate", PACT_MODE_TIERCE},
    {"--ldlat-period", PACT_MODE_TIERCE},
    {"--ldlat-dyn", PACT_MODE_TIERCE},
    {"--ldlat-dyn-tight", PACT_MODE_TIERCE},
    {"--ldlat-dyn-lo", PACT_MODE_TIERCE},
    {"--ldlat-dyn-emerg", PACT_MODE_TIERCE},
    {"--ldlat-dyn-decide", PACT_MODE_TIERCE},
    {"--ldlat-dyn-promo-min", PACT_MODE_TIERCE},
    {"--ldlat-dyn-alpha", PACT_MODE_TIERCE},
    {"--ldlat-dyn-start", PACT_MODE_TIERCE},
    {"--use-cat", PACT_MODE_TIERCE},
    {"--cat-signal", PACT_MODE_TIERCE},
    {"--cat-wmin", PACT_MODE_TIERCE},
    {"--cat-wmax", PACT_MODE_TIERCE},
    {"--cat-interval", PACT_MODE_TIERCE},
    {"--cat-tau-lo", PACT_MODE_TIERCE},
    {"--cat-tau-hi", PACT_MODE_TIERCE},
    {"--cat-probe-tol", PACT_MODE_TIERCE},
    {"--cat-probe-s", PACT_MODE_TIERCE},
    {"--cat-cooldown-s", PACT_MODE_TIERCE},
    {"--cat-bw-thresh", PACT_MODE_TIERCE},
    {"--cat-bw-band", PACT_MODE_TIERCE},
    {"--cat-bw-warmup-s", PACT_MODE_TIERCE},
    {"--cat-trace", PACT_MODE_TIERCE},
};
#define NR_MODE_OPTIONS (sizeof(mode_options) / sizeof(mode_options[0]))
_Static_assert(NR_MODE_OPTIONS <= 64, "mode option bits must fit in a uint64_t");

/* Record a mode-only option in the seen mask. */
static void note_mode_option(const char *arg, uint64_t *seen)
{
    for (size_t k = 0; k < NR_MODE_OPTIONS; k++) {
        if (strcmp(arg, mode_options[k].name) == 0) {
            *seen |= 1ULL << k;
            return;
        }
    }
}

/* After parsing: every mode-only option given must belong to the mode. */
static int check_mode_options(uint64_t seen, pact_mode_t mode)
{
    for (size_t k = 0; k < NR_MODE_OPTIONS; k++) {
        if ((seen & (1ULL << k)) && mode_options[k].mode != mode) {
            fprintf(stderr, "Error: %s is a %s-mode option; it is not valid with --mode %s\n",
                    mode_options[k].name, pact_mode_name(mode_options[k].mode),
                    pact_mode_name(mode));
            return -1;
        }
    }
    return 0;
}

/* After parsing: a sampling-period option must match the open event, and
 * the LDLAT gate needs LDLAT. */
static int check_sampling_options(const pact_config_t *config)
{
    if (config->use_ldlat && config->pebs_period != 0) {
        fprintf(stderr, "Error: --pebs-period sets the LLC-miss event, which --use-ldlat does "
                        "not open; use --ldlat-period\n");
        return -1;
    }
    if (!config->use_ldlat && config->ldlat_period != 0) {
        fprintf(stderr, "Error: --ldlat-period needs --use-ldlat\n");
        return -1;
    }
    if (!config->use_ldlat && config->ldlat_gate != 0) {
        fprintf(stderr, "Error: --ldlat-gate needs --use-ldlat\n");
        return -1;
    }
    const ldlat_dyn_config_t *dyn = &config->ldlat_dyn;
    if (config->ldlat_dyn_opt_seen && !dyn->enabled) {
        fprintf(stderr, "Error: --ldlat-dyn-* options need --ldlat-dyn\n");
        return -1;
    }
    if (dyn->enabled && !config->use_ldlat) {
        fprintf(stderr, "Error: --ldlat-dyn needs --use-ldlat\n");
        return -1;
    }
    if (dyn->enabled && config->ldlat_gate != 0 && config->ldlat_gate != LDLAT_HW_THRESHOLD) {
        fprintf(stderr,
                "Error: --ldlat-gate %u is not valid with --ldlat-dyn, which sets the gate from "
                "the open %u-cycle floor\n",
                config->ldlat_gate, LDLAT_HW_THRESHOLD);
        return -1;
    }
    if (dyn->emerg < dyn->lo) {
        fprintf(stderr, "Error: --ldlat-dyn-emerg (%g) must not be below --ldlat-dyn-lo (%g)\n",
                dyn->emerg, dyn->lo);
        return -1;
    }
    return 0;
}

/* After parsing: --cat-* options need --use-cat; an unset tau takes the
 * signal's default; tau_lo < tau_hi; explicit W_min <= W_max. */
static int check_cat_options(pact_config_t *config)
{
    cat_config_t *cat = &config->cat;
    if (config->cat_opt_seen && !cat->enabled) {
        fprintf(stderr, "Error: --cat-* options need --use-cat\n");
        return -1;
    }
    cat_resolve_taus(cat);
    if (cat->tau_lo >= cat->tau_hi) {
        fprintf(stderr, "Error: --cat-tau-lo (%g) must be below --cat-tau-hi (%g)\n", cat->tau_lo,
                cat->tau_hi);
        return -1;
    }
    if (cat->wmin > 0 && cat->wmax > 0 && cat->wmin > cat->wmax) {
        fprintf(stderr, "Error: --cat-wmin (%d) must not exceed --cat-wmax (%d)\n", cat->wmin,
                cat->wmax);
        return -1;
    }
    return 0;
}

/* Parse a non-negative integer option value in [lo, hi]. */
static int parse_bounded_uint(const char *opt, const char *arg, unsigned long lo, unsigned long hi,
                              unsigned long *out)
{
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);
    if (arg[0] < '0' || arg[0] > '9' || errno != 0 || *end != '\0' || value < lo || value > hi) {
        fprintf(stderr, "Error: %s must be an integer in [%lu, %lu]\n", opt, lo, hi);
        return -1;
    }
    *out = value;
    return 0;
}

/* Parse a number option value in [lo, hi] (lo excluded when lo_open). */
static int parse_bounded_double(const char *opt, const char *arg, double lo, double hi,
                                bool lo_open, double *out)
{
    errno = 0;
    char *end = NULL;
    double value = strtod(arg, &end);
    if (errno != 0 || end == arg || *end != '\0' || !isfinite(value) ||
        (lo_open ? value <= lo : value < lo) || value > hi) {
        fprintf(stderr, "Error: %s must be a number in %s%g, %g]\n", opt, lo_open ? "(" : "[", lo,
                hi);
        return -1;
    }
    *out = value;
    return 0;
}

/* Parse a CPU to pin to: -1 (no pinning) or a CPU number below
 * CPU_SETSIZE. */
static int parse_cpu_option(const char *opt, const char *arg, int *out)
{
    if (strcmp(arg, "-1") == 0) {
        *out = -1;
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long cpu = strtoul(arg, &end, 10);
    if (arg[0] < '0' || arg[0] > '9' || errno != 0 || *end != '\0' || cpu >= CPU_SETSIZE) {
        fprintf(stderr, "Error: %s must be -1 (no pinning) or a CPU number in [0, %d]\n", opt,
                CPU_SETSIZE - 1);
        return -1;
    }
    *out = (int)cpu;
    return 0;
}

/* --ldlat-dyn-* options. Returns 0, or -1 with a message. */
static int parse_ldlat_dyn_option(int argc, char *argv[], int *i, pact_config_t *config)
{
    const char *opt = argv[*i];
    ldlat_dyn_config_t *dyn = &config->ldlat_dyn;
    config->ldlat_dyn_opt_seen = true;
    if (++(*i) >= argc) {
        fprintf(stderr, "Error: %s requires a value\n", opt);
        return -1;
    }
    const char *arg = argv[*i];
    unsigned long n;
    if (strcmp(opt, "--ldlat-dyn-tight") == 0) {
        if (parse_bounded_uint(opt, arg, 400, 2000, &n) < 0) {
            return -1;
        }
        dyn->tight = (uint32_t)n;
        return 0;
    }
    if (strcmp(opt, "--ldlat-dyn-start") == 0) {
        if (strcmp(arg, "tight") != 0 && strcmp(arg, "open") != 0) {
            fprintf(stderr, "Error: --ldlat-dyn-start must be tight or open\n");
            return -1;
        }
        dyn->start_open = strcmp(arg, "open") == 0;
        return 0;
    }
    static const struct {
        const char *name;
        double lo, hi;
        bool lo_open;
        size_t offset;
    } numbers[] = {
        {"--ldlat-dyn-lo", 0, 1, false, offsetof(ldlat_dyn_config_t, lo)},
        {"--ldlat-dyn-emerg", 0, 1, false, offsetof(ldlat_dyn_config_t, emerg)},
        {"--ldlat-dyn-decide", 1, 600, false, offsetof(ldlat_dyn_config_t, decide_s)},
        {"--ldlat-dyn-promo-min", 0, 1e12, false, offsetof(ldlat_dyn_config_t, promo_min)},
        {"--ldlat-dyn-alpha", 0, 1, true, offsetof(ldlat_dyn_config_t, alpha)},
    };
    for (size_t k = 0; k < sizeof(numbers) / sizeof(numbers[0]); k++) {
        if (strcmp(opt, numbers[k].name) == 0) {
            double *field = (double *)((char *)dyn + numbers[k].offset);
            return parse_bounded_double(opt, arg, numbers[k].lo, numbers[k].hi, numbers[k].lo_open,
                                        field);
        }
    }
    fprintf(stderr, "Error: Unknown option: %s\n", opt);
    return -1;
}

/* Copy a path option value; an over-long path is an error. */
static int copy_path(const char *opt, const char *arg, char *dst, size_t len)
{
    if (strlen(arg) >= len) {
        fprintf(stderr, "Error: %s path is longer than %zu bytes\n", opt, len - 1);
        return -1;
    }
    strcpy(dst, arg);
    return 0;
}

#ifdef PACT_DIAG
/* Diagnostic options of a `make DIAG=1` build: --dump-pac-table,
 * --dump-pac-interval and --no-promote-wl. Returns 0, or -1 with a
 * message. */
static int parse_diag_option(int argc, char *argv[], int *i, pact_config_t *config)
{
    const char *opt = argv[*i];
    bool table = strcmp(opt, "--dump-pac-table") == 0;
    bool interval = strcmp(opt, "--dump-pac-interval") == 0;
    if (++(*i) >= argc) {
        fprintf(stderr, "Error: %s requires a value\n", opt);
        return -1;
    }
    unsigned long n;
    if (table) {
        return copy_path(opt, argv[*i], config->dump_pac_table, sizeof(config->dump_pac_table));
    }
    if (interval) {
        if (parse_bounded_uint(opt, argv[*i], 1, 86400, &n) < 0) {
            return -1;
        }
        config->dump_pac_interval_s = (uint32_t)n;
        return 0;
    }
    if (parse_bounded_uint(opt, argv[*i], 0, MAX_WORKLOADS - 1, &n) < 0) {
        return -1;
    }
    config->no_promote_mask |= 1u << n;
    return 0;
}

/* After parsing: the dump interval needs the dump, and --no-promote-wl
 * names a given workload. */
static int check_diag_options(const pact_config_t *config)
{
    if (config->dump_pac_interval_s != 0 && config->dump_pac_table[0] == '\0') {
        fprintf(stderr, "Error: --dump-pac-interval needs --dump-pac-table\n");
        return -1;
    }
    if (config->no_promote_mask >> config->nr_workloads) {
        fprintf(stderr, "Error: --no-promote-wl names a workload that no --workload gives\n");
        return -1;
    }
    return 0;
}
#endif

/* --cat-bw-thresh / --cat-bw-band: a GB/s value or "auto". */
static int parse_bw_value(const char *opt, const char *arg, double automatic, double *out)
{
    if (strcmp(arg, "auto") == 0) {
        *out = automatic;
        return 0;
    }
    return parse_bounded_double(opt, arg, 0, 1e6, true, out);
}

/* --cat-* options. Returns 0, or -1 with a message. */
static int parse_cat_option(int argc, char *argv[], int *i, pact_config_t *config)
{
    const char *opt = argv[*i];
    cat_config_t *cat = &config->cat;
    config->cat_opt_seen = true;
    if (++(*i) >= argc) {
        fprintf(stderr, "Error: %s requires a value\n", opt);
        return -1;
    }
    const char *arg = argv[*i];
    unsigned long n;
    if (strcmp(opt, "--cat-signal") == 0) {
        if (cat_signal_parse(arg, &cat->signal) != 0) {
            fprintf(stderr, "Error: --cat-signal must be mpki, l2lat, probe, bw or static\n");
            return -1;
        }
        return 0;
    }
    if (strcmp(opt, "--cat-wmin") == 0 || strcmp(opt, "--cat-wmax") == 0) {
        if (parse_bounded_uint(opt, arg, 1, 63, &n) < 0) {
            return -1;
        }
        *(strcmp(opt, "--cat-wmin") == 0 ? &cat->wmin : &cat->wmax) = (int)n;
        return 0;
    }
    if (strcmp(opt, "--cat-interval") == 0) {
        if (parse_bounded_uint(opt, arg, 10, 10000, &n) < 0) {
            return -1;
        }
        cat->interval_ms = (uint32_t)n;
        return 0;
    }
    if (strcmp(opt, "--cat-bw-thresh") == 0) {
        return parse_bw_value(opt, arg, 0.0, &cat->bw_thresh_gbps);
    }
    if (strcmp(opt, "--cat-bw-band") == 0) {
        return parse_bw_value(opt, arg, -1.0, &cat->bw_band_gbps);
    }
#ifdef PACT_DIAG
    if (strcmp(opt, "--cat-trace") == 0) {
        if (strlen(arg) >= sizeof(cat->trace_file)) {
            fprintf(stderr, "Error: --cat-trace path is too long\n");
            return -1;
        }
        strcpy(cat->trace_file, arg);
        return 0;
    }
#endif
    static const struct {
        const char *name;
        double lo, hi;
        bool lo_open;
        size_t offset;
    } numbers[] = {
        {"--cat-tau-lo", 0, 1e12, true, offsetof(cat_config_t, tau_lo)},
        {"--cat-tau-hi", 0, 1e12, true, offsetof(cat_config_t, tau_hi)},
        {"--cat-probe-tol", 0, 0.5, false, offsetof(cat_config_t, probe_tol)},
        {"--cat-probe-s", 0.1, 60, false, offsetof(cat_config_t, probe_s)},
        {"--cat-cooldown-s", 0, 60, false, offsetof(cat_config_t, cooldown_s)},
        {"--cat-bw-warmup-s", 1, 600, false, offsetof(cat_config_t, bw_warmup_s)},
    };
    for (size_t k = 0; k < sizeof(numbers) / sizeof(numbers[0]); k++) {
        if (strcmp(opt, numbers[k].name) == 0) {
            double *field = (double *)((char *)cat + numbers[k].offset);
            return parse_bounded_double(opt, arg, numbers[k].lo, numbers[k].hi, numbers[k].lo_open,
                                        field);
        }
    }
    fprintf(stderr, "Error: Unknown option: %s\n", opt);
    return -1;
}

/* Copy one optional --workload word; an over-long word is an error. */
static int copy_workload_word(const char *what, const char *word, char *dst, size_t len)
{
    if (strlen(word) >= len) {
        fprintf(stderr, "Error: --workload %s '%s' is longer than %zu bytes\n", what, word,
                len - 1);
        return -1;
    }
    strcpy(dst, word);
    return 0;
}

/* --workload PID [CPUS [NAME [CGROUP]]]: the optional words follow the PID
 * up to the next option. */
static int parse_workload_arg(int argc, char *argv[], int *i, pact_config_t *config)
{
    if (++(*i) >= argc) {
        fprintf(stderr, "Error: --workload requires a PID\n");
        return -1;
    }
    if (config->nr_workloads >= MAX_WORKLOADS) {
        fprintf(stderr, "Error: at most %d --workload options are supported\n", MAX_WORKLOADS);
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long pid = strtol(argv[*i], &end, 10);
    if (errno != 0 || end == argv[*i] || *end != '\0' || pid <= 0 || pid > INT_MAX) {
        fprintf(stderr, "Error: Invalid PID: %s\n", argv[*i]);
        return -1;
    }
    pact_workload_config_t *w = &config->workloads[config->nr_workloads];
    memset(w, 0, sizeof(*w));
    w->pid = (pid_t)pid;
    struct {
        const char *what;
        char *dst;
        size_t len;
    } words[] = {{"CPUS", w->cpus, sizeof(w->cpus)},
                 {"NAME", w->name, sizeof(w->name)},
                 {"CGROUP", w->cgroup, sizeof(w->cgroup)}};
    for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++) {
        if (*i + 1 >= argc || argv[*i + 1][0] == '-') {
            break;
        }
        (*i)++;
        if (copy_workload_word(words[k].what, argv[*i], words[k].dst, words[k].len) < 0) {
            return -1;
        }
    }
    if (w->name[0] == '\0') {
        snprintf(w->name, sizeof(w->name), "workload%d", config->nr_workloads);
    }
    printf("Workload %d: PID %d\n", config->nr_workloads, w->pid);
    config->nr_workloads++;
    return 0;
}

int pact_parse_command_line_args(int argc, char *argv[], pact_config_t *config)
{
    uint64_t seen = 0;
    for (int i = 1; i < argc; i++) {
        note_mode_option(argv[i], &seen);
        if (strcmp(argv[i], "--mode") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --mode requires an argument (pact or tierce)\n");
                return -1;
            }
            if (strcmp(argv[i], "pact") == 0) {
                config->mode = PACT_MODE_PACT;
            } else if (strcmp(argv[i], "tierce") == 0) {
                config->mode = PACT_MODE_TIERCE;
            } else {
                fprintf(stderr, "Error: --mode must be pact or tierce\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--domain-migration-batch") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --domain-migration-batch requires an argument\n");
                return -1;
            }
            unsigned long batch;
            if (parse_bounded_uint("--domain-migration-batch", argv[i], 1,
                                   MIGRATION_RING_DEFAULT_SIZE, &batch) < 0) {
                return -1;
            }
            config->domain_migration_batch = (uint32_t)batch;
        } else if (strcmp(argv[i], "--use-ldlat") == 0) {
            config->use_ldlat = true;
        } else if (strcmp(argv[i], "--ldlat-gate") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --ldlat-gate requires a cycle count\n");
                return -1;
            }
            if (strcmp(argv[i], "350") != 0 && strcmp(argv[i], "700") != 0) {
                fprintf(stderr, "Error: --ldlat-gate must be 350 or 700\n");
                return -1;
            }
            config->ldlat_gate = (uint32_t)strtoul(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--ldlat-period") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --ldlat-period requires an argument\n");
                return -1;
            }
            unsigned long period;
            if (parse_bounded_uint("--ldlat-period", argv[i], 1, 1000000, &period) < 0) {
                return -1;
            }
            config->ldlat_period = period;
        } else if (strcmp(argv[i], "--trace-migration") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --trace-migration requires a file path\n");
                return -1;
            }
            if (copy_path("--trace-migration", argv[i], config->trace_migration,
                          sizeof(config->trace_migration)) < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--use-cat") == 0) {
            config->cat.enabled = true;
        } else if (strncmp(argv[i], "--cat-", 6) == 0) {
            if (parse_cat_option(argc, argv, &i, config) < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--ldlat-dyn") == 0) {
            config->ldlat_dyn.enabled = true;
        } else if (strncmp(argv[i], "--ldlat-dyn-", 12) == 0) {
            if (parse_ldlat_dyn_option(argc, argv, &i, config) < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--sampling-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --sampling-interval requires an argument\n");
                return -1;
            }
            unsigned long ms;
            if (parse_bounded_uint("--sampling-interval", argv[i], 1, PACT_MAX_INTERVAL_MS, &ms) <
                0) {
                return -1;
            }
            config->sampling_interval_ms = (uint32_t)ms;
        } else if (strcmp(argv[i], "--adaptive-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --adaptive-interval requires an argument\n");
                return -1;
            }
            unsigned long ms;
            if (parse_bounded_uint("--adaptive-interval", argv[i], 1, PACT_MAX_INTERVAL_MS, &ms) <
                0) {
                return -1;
            }
            config->adaptive_interval_ms = (uint32_t)ms;
        } else if (strcmp(argv[i], "--stats-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --stats-interval requires an argument\n");
                return -1;
            }
            unsigned long ms;
            if (parse_bounded_uint("--stats-interval", argv[i], 1, PACT_MAX_INTERVAL_MS, &ms) < 0) {
                return -1;
            }
            config->stats_interval_ms = (uint32_t)ms;
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
            if (parse_bounded_double("--cooling-alpha", argv[i], 0.0, 1.0, false,
                                     &config->cooling_alpha) < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--cooling-trigger-samples") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --cooling-trigger-samples requires an argument\n");
                return -1;
            }
            unsigned long samples;
            if (parse_bounded_uint("--cooling-trigger-samples", argv[i], 1, ULONG_MAX, &samples) <
                0) {
                return -1;
            }
            config->cooling_trigger_samples = samples;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--logging") == 0) {
            config->enable_logging = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                if (strcmp(argv[i], "csv") == 0) {
                    strncpy(config->log_format, argv[i], sizeof(config->log_format) - 1);
                    config->log_format[sizeof(config->log_format) - 1] = '\0';
                } else if (strcmp(argv[i], "json") == 0) {
                    fprintf(stderr, "Error: --logging json is not supported. The only log "
                                    "format is csv.\n");
                    return -1;
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
            if (parse_workload_arg(argc, argv, &i, config) < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--log-level") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --log-level requires an argument\n");
                return -1;
            }
            unsigned long log_level;
            if (parse_bounded_uint("--log-level", argv[i], 0, 4, &log_level) < 0) {
                return -1;
            }
            set_log_level((int)log_level);
            printf("Log level set to %lu\n", log_level);
        } else if (strcmp(argv[i], "--monitor-cpu") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --monitor-cpu requires an argument\n");
                return -1;
            }
            if (parse_cpu_option("--monitor-cpu", argv[i], &config->monitor_cpu) < 0) {
                return -1;
            }
            printf("Monitor CPU affinity set to %d\n", config->monitor_cpu);
        } else if (strcmp(argv[i], "--migration-cpu") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --migration-cpu requires an argument\n");
                return -1;
            }
            if (parse_cpu_option("--migration-cpu", argv[i], &config->migration_cpu) < 0) {
                return -1;
            }
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
            errno = 0;
            char *end = NULL;
            unsigned long long limit = strtoull(argv[i], &end, 10);
            if (argv[i][0] < '0' || argv[i][0] > '9' || *end != '\0' || errno != 0 ||
                limit > SIZE_MAX) {
                fprintf(stderr, "Error: --pac-pool-max must be a non-negative integer\n");
                return -1;
            }
            config->pac_pool_max = (size_t)limit;
            if (limit == 0) {
                printf("PAC metadata pool cap: unlimited\n");
            } else {
                printf("PAC metadata pool cap: %zu entries\n", config->pac_pool_max);
            }
#ifdef PACT_DIAG
        } else if (strcmp(argv[i], "--dump-pac-table") == 0 ||
                   strcmp(argv[i], "--dump-pac-interval") == 0 ||
                   strcmp(argv[i], "--no-promote-wl") == 0) {
            if (parse_diag_option(argc, argv, &i, config) < 0) {
                return -1;
            }
#endif
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

    if (config->nr_workloads == 0) {
        pact_print_usage(argv[0]);
        fprintf(stderr, "\nError: --workload PID CPUS NAME CGROUP is required.\n");
        return -1;
    }
    if (check_mode_options(seen, config->mode) < 0 || check_sampling_options(config) < 0 ||
        check_cat_options(config) < 0) {
        return -1;
    }
#ifdef PACT_DIAG
    if (check_diag_options(config) < 0) {
        return -1;
    }
#endif
    return 0;
}
