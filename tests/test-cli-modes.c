/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* CLI: --mode, per-mode defaults, mode-owned options, value checks of
 * shared options, and the --workload rules of each mode, through the actual
 * parser and validator. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/cli.h"
#include "../src/error.h"
#include "../src/validate.h"

void pact_print_usage(const char *name)
{
    (void)name;
}
void pact_print_version(void) {}
void pact_signal_set_crash_marker_path(const char *path)
{
    (void)path;
}

/* Parse a NULL-terminated argument list into a fresh config. */
static int parse(pact_config_t *config, ...)
{
    char *argv[64] = {"pact"};
    int argc = 1;
    va_list args;
    va_start(args, config);
    for (char *arg = va_arg(args, char *); arg; arg = va_arg(args, char *)) {
        assert(argc < 64);
        argv[argc++] = arg;
    }
    va_end(args);
    pact_init_config(config);
    return pact_parse_command_line_args(argc, argv, config);
}

#define WL "--workload", "1", "2-3", "fg", "fg-cgroup"

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    pact_config_t c;

    /* Mode and the per-mode PEBS period default. */
    assert(parse(&c, WL, NULL) == 0);
    assert(c.mode == PACT_MODE_PACT && pact_config_pebs_period(&c) == 400);
    assert(parse(&c, "--mode", "tierce", WL, NULL) == 0);
    assert(c.mode == PACT_MODE_TIERCE && pact_config_pebs_period(&c) == 100);
    assert(parse(&c, "--mode", "tierce", "--pebs-period", "7", WL, NULL) == 0);
    assert(pact_config_pebs_period(&c) == 7);
    assert(parse(&c, "--mode", "pact", WL, NULL) == 0 && c.mode == PACT_MODE_PACT);
    assert(parse(&c, "--mode", "other", WL, NULL) < 0);
    assert(parse(&c, "--mode", NULL) < 0);

    /* Timing options: whole milliseconds from 1 to one hour. */
    assert(parse(&c, WL, "--sampling-interval", "20", "--adaptive-interval", "100",
                 "--stats-interval", "1000", NULL) == 0);
    assert(c.sampling_interval_ms == 20 && c.adaptive_interval_ms == 100 &&
           c.stats_interval_ms == 1000);
    assert(parse(&c, WL, "--sampling-interval", "0", NULL) < 0);
    assert(parse(&c, WL, "--sampling-interval", "-5", NULL) < 0);
    assert(parse(&c, WL, "--adaptive-interval", "20ms", NULL) < 0);
    assert(parse(&c, WL, "--stats-interval", "3600001", NULL) < 0);

    /* A flag of the other mode is an error, before or after --mode. */
    assert(parse(&c, "--bin-count", "5", "--mode", "tierce", WL, NULL) < 0);
    assert(parse(&c, "--mode", "tierce", WL, "--bin-width", "10", NULL) < 0);
    assert(parse(&c, "--mode", "tierce", WL, "--max-migrations-per-cycle", "8", NULL) < 0);
    assert(parse(&c, WL, "--domain-migration-batch", "8", NULL) < 0);
    assert(parse(&c, WL, "--bin-count", "5", "--max-migrations-per-cycle", "8", NULL) == 0);
    assert(c.bin_count == 5 && c.max_migrations_per_cycle == 8);

    /* --domain-migration-batch: 1 to the migration ring size. */
    assert(parse(&c, "--mode", "tierce", WL, NULL) == 0 && c.domain_migration_batch == 32);
    assert(parse(&c, "--mode", "tierce", WL, "--domain-migration-batch", "1", NULL) == 0);
    assert(c.domain_migration_batch == 1);
    assert(parse(&c, "--mode", "tierce", WL, "--domain-migration-batch", "65536", NULL) == 0);
    assert(parse(&c, "--mode", "tierce", WL, "--domain-migration-batch", "0", NULL) < 0);
    assert(parse(&c, "--mode", "tierce", WL, "--domain-migration-batch", "65537", NULL) < 0);
    assert(parse(&c, "--mode", "tierce", WL, "--domain-migration-batch", "4x", NULL) < 0);

    /* LDLAT options belong to Tierce mode. --ldlat-period and --ldlat-gate
     * need --use-ldlat; --pebs-period is not valid with it. The gate is 700
     * by default, 350 with --ldlat-dyn, and 0 (no gate) without LDLAT. */
    assert(parse(&c, WL, NULL) == 0 && pact_config_ldlat_gate(&c) == 0);
    assert(parse(&c, "--mode", "tierce", WL, NULL) == 0 && pact_config_ldlat_gate(&c) == 0);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", WL, NULL) == 0);
    assert(c.use_ldlat && pact_config_ldlat_gate(&c) == 700 && pact_config_pebs_period(&c) == 5);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-gate", "350", WL, NULL) == 0);
    assert(pact_config_ldlat_gate(&c) == 350 && pact_config_pebs_period(&c) == 5);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-gate", "700", WL, NULL) == 0);
    assert(pact_config_ldlat_gate(&c) == 700);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-period", "9", "--ldlat-gate",
                 "350", WL, NULL) == 0);
    assert(pact_config_pebs_period(&c) == 9 && pact_config_ldlat_gate(&c) == 350);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--pebs-period", "100", WL, NULL) < 0);
    assert(parse(&c, "--mode", "tierce", "--ldlat-period", "5", WL, NULL) < 0);
    assert(parse(&c, "--mode", "tierce", "--ldlat-gate", "700", WL, NULL) < 0);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-gate", "500", WL, NULL) < 0);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-period", "0", WL, NULL) < 0);
    assert(parse(&c, "--use-ldlat", WL, NULL) < 0);
    assert(parse(&c, "--ldlat-gate", "700", WL, NULL) < 0);
    assert(parse(&c, "--ldlat-gate", "350", WL, NULL) < 0);
    assert(parse(&c, "--use-ldlat", "--ldlat-gate", "700", WL, NULL) < 0);
    assert(parse(&c, "--ldlat-period", "5", WL, NULL) < 0);

    /* LDLAT-Dyn: needs --use-ldlat; it switches its own gate between open
     * (350) and tight, so the static gate default is 350, an explicit 350 is
     * accepted, and 700 is an error in either order. Its options need it. */
#define DYN "--mode", "tierce", "--use-ldlat", "--ldlat-dyn"
    assert(parse(&c, DYN, WL, NULL) == 0);
    assert(c.ldlat_dyn.enabled && c.ldlat_dyn.tight == 700 && c.ldlat_dyn.alpha == 0.2);
    assert(pact_config_ldlat_gate(&c) == 350 && pact_config_pebs_period(&c) == 5);
    assert(parse(&c, DYN, "--ldlat-gate", "350", WL, NULL) == 0);
    assert(pact_config_ldlat_gate(&c) == 350);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-gate", "350", "--ldlat-dyn", WL,
                 NULL) == 0);
    assert(pact_config_ldlat_gate(&c) == 350);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-gate", "700", "--ldlat-dyn", WL,
                 NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-tight", "900", "--ldlat-dyn-lo", "0.001",
                 "--ldlat-dyn-emerg", "0.5", "--ldlat-dyn-decide", "10", "--ldlat-dyn-promo-min",
                 "5", "--ldlat-dyn-alpha", "1", "--ldlat-dyn-start", "open", WL, NULL) == 0);
    assert(c.ldlat_dyn.tight == 900 && c.ldlat_dyn.lo == 0.001 && c.ldlat_dyn.emerg == 0.5);
    assert(c.ldlat_dyn.decide_s == 10 && c.ldlat_dyn.promo_min == 5 && c.ldlat_dyn.alpha == 1);
    assert(c.ldlat_dyn.start_open);
    assert(parse(&c, "--mode", "tierce", "--ldlat-dyn", WL, NULL) < 0);
    assert(parse(&c, "--mode", "tierce", "--use-ldlat", "--ldlat-dyn-lo", "0.1", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-gate", "700", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-lo", "0.1", "--ldlat-dyn-emerg", "0.05", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-tight", "300", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-alpha", "0", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-start", "half", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-decide", "nan", WL, NULL) < 0);
    assert(parse(&c, DYN, "--ldlat-dyn-other", "1", WL, NULL) < 0);
    assert(parse(&c, "--ldlat-dyn", WL, NULL) < 0);

    /* CAT options belong to Tierce mode and need --use-cat. */
#define CAT "--mode", "tierce", "--use-cat"
    assert(parse(&c, CAT, WL, NULL) == 0);
    assert(c.cat.enabled && c.cat.signal == CAT_SIGNAL_MPKI && c.cat.interval_ms == 100);
    assert(c.cat.tau_lo == 10.0 && c.cat.tau_hi == 25.0 && c.cat.wmin == -1);
    assert(parse(&c, CAT, "--cat-signal", "l2lat", WL, NULL) == 0);
    assert(c.cat.tau_lo == 110.0 && c.cat.tau_hi == 140.0);
    assert(parse(&c, CAT, "--cat-signal", "probe", "--cat-wmin", "3", "--cat-wmax", "8",
                 "--cat-interval", "50", "--cat-tau-lo", "5", "--cat-tau-hi", "30",
                 "--cat-probe-tol", "0.1", "--cat-probe-s", "1", "--cat-cooldown-s", "0",
                 "--cat-bw-thresh", "4.5", "--cat-bw-band", "auto", "--cat-bw-warmup-s", "10", WL,
                 NULL) == 0);
    assert(c.cat.signal == CAT_SIGNAL_PROBE && c.cat.wmin == 3 && c.cat.wmax == 8);
    assert(c.cat.interval_ms == 50 && c.cat.tau_lo == 5 && c.cat.tau_hi == 30);
    assert(c.cat.probe_tol == 0.1 && c.cat.probe_s == 1);
    assert(c.cat.cooldown_s == 0 && c.cat.bw_thresh_gbps == 4.5 && c.cat.bw_band_gbps == -1.0);
    assert(c.cat.bw_warmup_s == 10);
    assert(parse(&c, "--mode", "tierce", "--cat-signal", "mpki", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-signal", "ipc", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-tau-lo", "30", "--cat-tau-hi", "20", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-wmin", "9", "--cat-wmax", "8", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-wmin", "0", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-interval", "5", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-bw-band", "0", WL, NULL) < 0);
    assert(parse(&c, CAT, "--cat-fg-ways", "8", WL, NULL) < 0);
    assert(parse(&c, "--use-cat", WL, NULL) < 0);
#ifdef PACT_DIAG
    assert(parse(&c, CAT, "--cat-trace", "cat.csv", WL, NULL) == 0);
    assert(strcmp(c.cat.trace_file, "cat.csv") == 0);
#else
    assert(parse(&c, CAT, "--cat-trace", "cat.csv", WL, NULL) < 0);
#endif

    /* --trace-migration is shared by both modes. */
    assert(parse(&c, WL, "--trace-migration", "trace.csv", NULL) == 0);
    assert(strcmp(c.trace_migration, "trace.csv") == 0);
    assert(parse(&c, "--mode", "tierce", WL, "--trace-migration", "t.csv", NULL) == 0);
    assert(parse(&c, WL, "--trace-migration", NULL) < 0);
#ifdef PACT_DIAG
    /* Diagnostic options (shared): the interval needs the dump, and
     * --no-promote-wl names a given workload. */
    assert(parse(&c, WL, "--dump-pac-table", "dump.csv", "--dump-pac-interval", "5", NULL) == 0);
    assert(strcmp(c.dump_pac_table, "dump.csv") == 0 && c.dump_pac_interval_s == 5);
    assert(parse(&c, WL, "--dump-pac-interval", "5", NULL) < 0);
    assert(parse(&c, WL, "--dump-pac-table", "d.csv", "--dump-pac-interval", "0", NULL) < 0);
    assert(parse(&c, "--mode", "tierce", WL, "--workload", "12", "4-5", "bg", "bg-cg",
                 "--no-promote-wl", "1", "--no-promote-wl", "0", NULL) == 0);
    assert(c.no_promote_mask == 3);
    assert(parse(&c, WL, "--no-promote-wl", "1", NULL) < 0);
    assert(parse(&c, WL, "--no-promote-wl", "16", NULL) < 0);
#else
    assert(parse(&c, WL, "--dump-pac-table", "dump.csv", NULL) < 0);
    assert(parse(&c, WL, "--no-promote-wl", "0", NULL) < 0);
#endif

    /* --cooling-alpha: the whole value must be a number in [0, 1]. */
    assert(parse(&c, WL, "--cooling-alpha", "0.5", NULL) == 0 && c.cooling_alpha == 0.5);
    assert(parse(&c, WL, "--cooling-alpha", "abc", NULL) < 0);
    assert(parse(&c, WL, "--cooling-alpha", "0.5x", NULL) < 0);
    assert(parse(&c, WL, "--cooling-alpha", "1.5", NULL) < 0);
    assert(parse(&c, WL, "--cooling-alpha", "nan", NULL) < 0);

    /* --cooling-trigger-samples: the whole value must be an integer >= 1. */
    assert(parse(&c, WL, "--cooling-trigger-samples", "5000", NULL) == 0);
    assert(c.cooling_trigger_samples == 5000);
    assert(parse(&c, WL, "--cooling-trigger-samples", "abc", NULL) < 0);
    assert(parse(&c, WL, "--cooling-trigger-samples", "1x", NULL) < 0);
    assert(parse(&c, WL, "--cooling-trigger-samples", "0", NULL) < 0);
    assert(parse(&c, WL, "--cooling-trigger-samples", "-1", NULL) < 0);

    /* --log-level: the whole value must be an integer in [0, 4]. */
    assert(parse(&c, WL, "--log-level", "0", NULL) == 0);
    assert(parse(&c, WL, "--log-level", "abc", NULL) < 0);
    assert(parse(&c, WL, "--log-level", "1x", NULL) < 0);
    assert(parse(&c, WL, "--log-level", "5", NULL) < 0);
    assert(parse(&c, WL, "--log-level", "-1", NULL) < 0);

    /* The only log format is csv. */
    assert(parse(&c, WL, "--logging", "csv", NULL) == 0 && strcmp(c.log_format, "csv") == 0);
    assert(parse(&c, WL, "--logging", "json", NULL) < 0);

    /* --monitor-cpu and --migration-cpu: -1 or a CPU number. */
    assert(parse(&c, WL, "--monitor-cpu", "3", "--migration-cpu", "-1", NULL) == 0);
    assert(c.monitor_cpu == 3 && c.migration_cpu == -1);
    assert(parse(&c, WL, "--monitor-cpu", "abc", NULL) < 0);
    assert(parse(&c, WL, "--monitor-cpu", "-2", NULL) < 0);
    assert(parse(&c, WL, "--migration-cpu", "1x", NULL) < 0);
    assert(parse(&c, WL, "--migration-cpu", "", NULL) < 0);

    /* --workload words stop at the next option; NAME defaults by order. */
    assert(parse(&c, "--workload", "11", "auto", "--workload", "12", NULL) == 0);
    assert(c.nr_workloads == 2 && c.workloads[0].pid == 11 && c.workloads[1].pid == 12);
    assert(strcmp(c.workloads[0].cpus, "auto") == 0 && c.workloads[0].cgroup[0] == '\0');
    assert(strcmp(c.workloads[1].name, "workload1") == 0);

    /* Validation. CGROUP is required in both modes. */
    assert(parse(&c, "--workload", "11", "2-3", "a", NULL) == 0);
    assert(pact_validate_configuration(&c) < 0);
    /* PACT accepts auto (the process affinity); Tierce needs a CPU list. */
    assert(parse(&c, "--workload", "11", "auto", "a", "cg", NULL) == 0);
    assert(pact_validate_configuration(&c) == 0);
    assert(parse(&c, "--mode", "tierce", "--workload", "11", "auto", "a", "cg", NULL) == 0);
    assert(pact_validate_configuration(&c) < 0);
    /* Tierce CPU lists must be disjoint; PACT lists may overlap. */
    assert(parse(&c, "--mode", "tierce", WL, "--workload", "12", "3-4", "bg", "bg-cg", NULL) == 0);
    assert(pact_validate_configuration(&c) < 0);
    assert(parse(&c, "--mode", "tierce", WL, "--workload", "12", "4-5", "bg", "bg-cg", NULL) == 0);
    assert(pact_validate_configuration(&c) == 0);
    assert(parse(&c, WL, "--workload", "12", "3-4", "bg", "bg-cg", NULL) == 0);
    assert(pact_validate_configuration(&c) == 0);
    /* The same PID twice is an error. */
    assert(parse(&c, WL, "--workload", "1", "4-5", "bg", "bg-cg", NULL) == 0);
    assert(pact_validate_configuration(&c) < 0);
    return 0;
}
