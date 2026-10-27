/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* usage.c: CLI usage/help printer. */

#include <stdio.h>

#include "usage.h"
#include "config.h"    /* default values */
#include "constants.h" /* MIGRATION_RING_DEFAULT_SIZE */
#include "pact.h"      /* MAX_WORKLOADS */
#include "build-info.h"

void pact_print_version(void)
{
    printf("pact %s (built %s)\n", PACT_GIT_VERSION, PACT_BUILD_DATE);
}

void pact_print_usage(const char *prog_name)
{
    printf("PACT: A Criticality-First Design for Tiered Memory\n");
    printf("A userspace runtime that places pages by *performance criticality*, "
           "not just access\nfrequency, across a fast (local DRAM) and a slow "
           "(CXL/remote) memory tier.\n");

    printf("\n");
    printf("How it works (per ~20 ms cycle, all in user space):\n");
    printf("  1. SAMPLE   PEBS records which pages the workload loads from the "
           "slow tier, and\n");
    printf("              CHA/uncore counters measure memory-level parallelism "
           "(MLP).\n");
    printf("  2. SCORE    Each page gets a PAC (Per-page Access Criticality) value, "
           "~ stalled cycles\n");
    printf("              attributable to it (LLC misses weighted by tier latency / "
           "MLP). High\n");
    printf("              PAC = stalling the CPU, regardless of how often it is "
           "touched.\n");
    printf("  3. BIN      Pages are grouped into PAC bins (Freedman-Diaconis width, "
           "self-tuning).\n");
    printf("              Tierce mode instead sets each workload's bar to the 0.90 quantile "
           "of a\n");
    printf("              sample of its pages at the end of each sampling window. Without "
           "LDLAT the\n");
    printf("              bar counts every page of the sample; with LDLAT only the pages on "
           "the slow\n");
    printf("              tier (the runtime asks the kernel for the node of the others).\n");
    printf("  4. MIGRATE  The highest-criticality pages are promoted to the fast "
           "tier; cold pages\n");
    printf("              fall back to the slow tier. Repeat.\n");
    printf("\nSteps 1-3 run as coroutines on a single thread, plus a migration "
           "thread, all on\none dedicated core: PACT manages the whole workload "
           "at a cost of one core.\n");

    printf("\nUsage: %s --workload PID CPUS NAME CGROUP [--workload ...] [--mode pact|tierce] "
           "[options]\n",
           prog_name);
    printf("PACT attaches to already-running, externally CPU-pinned workloads "
           "and must run\nas root (PMU/PEBS access). See run/run.sh for the "
           "full experiment harness.\n");

    printf("\nRequired:\n");
    printf("  --workload PID CPUS NAME CGROUP\n");
    printf("                                    Process to manage; repeat for up to %d "
           "workloads.\n",
           MAX_WORKLOADS);
    printf("                                    CPUS: CPU list (e.g. 2-5,8). PACT mode: the "
           "CPUs to\n");
    printf("                                    monitor, or auto reads the process affinity. "
           "Tierce mode:\n");
    printf("                                    required, and the lists must not overlap. NAME: "
           "a label.\n");
    printf("                                    CGROUP (required): the workload's own "
           "cgroup, absolute\n");
    printf("                                    or under /sys/fs/cgroup, with a finite "
           "memory.max_at_node0.\n");
    printf("                                    memory.mmctl_enabled and memory.demotion must "
           "be enabled.\n");

    printf("\nMode:\n");
    printf("  --mode pact|tierce                pact (default): one PAC table, threshold and "
           "migration\n");
    printf("                                    queue for all workloads, scored with one "
           "global MLP.\n");
    printf("                                    tierce: one PAC table, threshold and queue per "
           "workload\n");
    printf("                                    (a domain), scored with the workload's MLP. "
           "An option of\n");
    printf("                                    the other mode is an error.\n");

    printf("\nPAC + migration tuning (both modes):\n");
    printf("  --pebs-period N                   PEBS sample period: 1 sample per N slow-tier "
           "LLC-miss\n");
    printf("                                    loads (default %u in PACT mode, %u in Tierce "
           "mode).\n",
           PACT_PEBS_SAMPLE_PERIOD, TIERCE_PEBS_SAMPLE_PERIOD);
    printf("                                    Not valid with --use-ldlat, which opens another "
           "event.\n");
    printf("  --cooling-alpha A                 PAC EWMA decay in [0,1]; 1.0 "
           "disables cooling (default 1.0).\n");
    printf("  --cooling-trigger-samples N       Samples applied to one PAC table before that "
           "table cools\n");
    printf("                                    (default 200000). PACT: one table for all "
           "workloads;\n");
    printf("                                    Tierce: one table per workload.\n");
    printf("  --pac-pool-max N                  Max tracked pages per PAC table (default 0 = "
           "unlimited).\n");
    printf("                                    PACT: one table for all workloads; Tierce: one "
           "table per\n");
    printf("                                    workload. With 0 the tables grow with no bound: "
           "each sampled\n");
    printf("                                    4 KiB page takes about 70-90 bytes, freed only at "
           "exit.\n");

    printf("\nPACT-mode options:\n");
    printf("  --max-migrations-per-cycle N      Most pages per numa_move_pages call; "
           "the migration\n");
    printf("                                    worker drains the whole queue on each "
           "wake (default %u).\n",
           PACT_DEFAULT_MAX_MIGRATIONS_PER_CYCLE);
    printf("  --bin-count N                     Number of PAC bins; the top bin is "
           "promoted\n");
    printf("                                    (default 20).\n");
    printf("  --bin-width W                     Initial PAC bin width; self-tunes at "
           "runtime (default 1000.0).\n");

    printf("\nTierce-mode options:\n");
    printf("  --domain-migration-batch N        Most pages one workload moves per round-robin "
           "turn, in\n");
    printf("                                    one numa_move_pages call, 1-%u (default %u).\n",
           MIGRATION_RING_DEFAULT_SIZE, PACT_DEFAULT_DOMAIN_MIGRATION_BATCH);
    printf("  --use-ldlat                       Sample loads above %u cycles (LDLAT) instead of "
           "LLC-miss\n",
           LDLAT_HW_THRESHOLD);
    printf("                                    loads; keep remote-DRAM loads and weight each "
           "sample by\n");
    printf("                                    its latency over the smoothed mean latency "
           "(k_slow, default off).\n");
    printf("  --ldlat-period N                  With --use-ldlat: 1 sample per N such loads "
           "(default %u).\n",
           TIERCE_LDLAT_SAMPLE_PERIOD);
    printf("  --ldlat-gate N                    With --use-ldlat: keep only samples of at least "
           "N cycles,\n");
    printf("                                    350 or 700 (default %u, the paper's gate). 350 "
           "keeps every\n",
           TIERCE_LDLAT_GATE);
    printf("                                    sample above the hardware floor. With "
           "--ldlat-dyn the default\n");
    printf("                                    is %u and %u is an error (LDLAT-Dyn sets the "
           "gate).\n",
           LDLAT_HW_THRESHOLD, TIERCE_LDLAT_GATE);
    printf("  --ldlat-dyn                       With --use-ldlat: switch the software gate "
           "between open (%u)\n",
           LDLAT_HW_THRESHOLD);
    printf("                                    and --ldlat-dyn-tight as each workload cgroup's "
           "fast-tier\n");
    printf("                                    eviction pressure changes (LDLAT-Dyn; default "
           "off).\n");
    printf("  --ldlat-dyn-tight CYC             Gate in the tight mode, 400-2000 (default 700).\n");
    printf("  --ldlat-dyn-lo E                  Pressure that tightens at a decision, 0-1 "
           "(default 0.002).\n");
    printf("  --ldlat-dyn-emerg E               Pressure that tightens at any 1 s tick, "
           "--ldlat-dyn-lo to 1\n");
    printf("                                    (default 0.015).\n");
    printf("  --ldlat-dyn-decide S              Decision period in seconds, 1-600 (default "
           "30).\n");
    printf("  --ldlat-dyn-promo-min R           Promotions/s needed to reopen, >= 0 (default "
           "200).\n");
    printf("  --ldlat-dyn-alpha A               EWMA weight of the newest pressure sample of "
           "each workload,\n");
    printf("                                    0 < A <= 1 (default 0.2).\n");
    printf("  --ldlat-dyn-start tight|open      Initial mode (default tight).\n");
    printf("  --use-cat                         Control the LLC ways of the first --workload (the "
           "foreground,\n");
    printf("                                    FG) through Intel CAT: the FG keeps W_min..W_max "
           "ways, the\n");
    printf("                                    later workloads share the other ways (default "
           "off).\n");
    printf("  --cat-signal NAME                 Demand signal: mpki (LLC misses per 1000 "
           "instructions),\n");
    printf("                                    l2lat (cycles per L2-missing demand load; needs "
           "more than 4\n");
    printf("                                    general-purpose counters per CPU; Skylake-SP "
           "reports 4, also\n");
    printf("                                    with SMT turned off at run time), probe (trial "
           "withdrawal of\n");
    printf("                                    one way judged on FG IPC), bw (FG memory "
           "bandwidth from\n");
    printf("                                    resctrl), static (hold W_max) (default mpki).\n");
    printf("  --cat-wmin N                      Lower bound of the FG ways (default (ways+2)/4: "
           "3 of 11,\n");
    printf("                                    5 of 20).\n");
    printf("  --cat-wmax N                      Upper bound of the FG ways (default 3*ways/4: 8 "
           "of 11,\n");
    printf("                                    15 of 20).\n");
    printf("  --cat-interval MS                 Control interval, 10-10000 (default 100).\n");
    printf("  --cat-tau-lo X                    Release one way below X (default 10 for mpki, "
           "110 cycles\n");
    printf("                                    for l2lat).\n");
    printf("  --cat-tau-hi X                    Restore W_max above X; tau-lo < tau-hi (default "
           "25 for\n");
    printf("                                    mpki, 140 cycles for l2lat).\n");
    printf("  --cat-probe-tol F                 probe: IPC drop that marks a way as needed, "
           "0-0.5\n");
    printf("                                    (default 0.03).\n");
    printf("  --cat-probe-s S                   probe: seconds between trial withdrawals, "
           "0.1-60 (default 2).\n");
    printf("  --cat-cooldown-s S                probe: hold after a restore, 0-60 (default "
           "3).\n");
    printf("  --cat-bw-thresh G|auto            bw: gate in GB/s; auto = midpoint of the FG "
           "bandwidth range\n");
    printf("                                    over the warmup (default auto).\n");
    printf("  --cat-bw-band G|auto              bw: hysteresis band in GB/s; auto = 0.15 x the "
           "warmup range,\n");
    printf("                                    or 0.1 x an explicit gate (default auto).\n");
    printf("  --cat-bw-warmup-s S               bw: warmup in seconds, 1-600 (default 25).\n");
#ifdef PACT_DIAG
    printf("  --cat-trace FILE                  Write the controller's decisions as CSV.\n");
#endif

    printf("\nTiming (milliseconds):\n");
    printf("  --sampling-interval MS            Sampling cadence (default 20). Tierce mode: the "
           "PAC epoch,\n");
    printf("                                    from the start of one window to the start of "
           "the next.\n");
    printf("  --adaptive-interval MS            How often the runtime drains the PAC "
           "update ring and\n");
    printf("                                    applies the samples (default 20). Tierce mode "
           "also drains it\n");
    printf("                                    right after each sampling window, whatever "
           "this value.\n");
    printf("  --stats-interval MS               Stats dump cadence (default 5000).\n");
    printf("                                    Each of the three takes 1 to 3600000 ms.\n");

    printf("\nCPU affinity:\n");
    printf("  --monitor-cpu CPU                 Pin the PACT event loop "
           "(-1 = none, default -1).\n");
    printf("  --migration-cpu CPU               Pin the migration thread "
           "(-1 = none, default -1).\n");

    printf("\nDiagnostics:\n");
    printf("  --trace-migration FILE            Write promotion and demotion counts and rates "
           "every second\n");
    printf("                                    as CSV, in total and per workload, with each "
           "workload\n");
    printf("                                    cgroup's kernel demotions.\n");
#ifdef PACT_DIAG
    printf("  --dump-pac-table FILE             Write snapshots of every PAC table with its "
           "admission bar\n");
    printf("                                    as CSV (this build has make DIAG=1).\n");
    printf("  --dump-pac-interval SEC           Snapshot period, 1-86400 (default %u).\n",
           PAC_DUMP_DEFAULT_INTERVAL_S);
    printf("  --no-promote-wl IDX               Never promote workload IDX (--workload order); "
           "repeatable.\n");
#endif
    printf("  --crash-marker PATH               Write an ok/crash marker file "
           "on exit.\n");
    printf("  -l, --logging [FORMAT]            Enable logging. FORMAT is csv (the default). "
           "Needs a\n");
    printf("                                    `make logging` build.\n");
    printf("  --log-file PATH                   Log file path.\n");
    printf("  --log-level LEVEL                 0=error 1=warn 2=info 3=debug 4=trace "
           "(default 2).\n");
    printf("  -h, --help                        Show this help and exit.\n");
    printf("  -V, --version                     Print version and exit.\n");

    printf("\nExamples:\n");
    printf("  sudo numactl -C 1 %s --workload $(pgrep bc) 2-5 bc pact/bc "
           "--max-migrations-per-cycle 4096\n",
           prog_name);
    printf("  sudo numactl -C 1 %s --mode tierce --workload $(pgrep fg) 2-5 fg pact/fg "
           "--workload $(pgrep bg) 6-9 bg pact/bg --use-ldlat\n",
           prog_name);
}
