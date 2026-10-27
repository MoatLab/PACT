/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* perf.c: perf event setup at init time.
 *
 * setup_* helpers (setup_counting_event, setup_pebs_event,
 * setup_pmu_cha_perf_events, setup_workload_counting_events,
 * start_pmu_perf_events, discover_cha_pmus) live in pmu.c, included via pmu.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "pact.h"
#include "config.h" /* PACT_PEBS_SAMPLE_PERIOD */
#include "pmu.h"
#include "perf.h"

#ifndef PEBS_SAMPLE_PERIOD
#define PEBS_SAMPLE_PERIOD PACT_PEBS_SAMPLE_PERIOD
#endif

void init_perf_event(perf_event_t *perf_event)
{
    perf_event->fd = -1;
    perf_event->id = -1;
    perf_event->value = 0;
    perf_event->time_enabled = 0;
    perf_event->time_running = 0;
}

void init_per_cpu_state(per_cpu_state_t *cpu_state)
{
    cpu_state->cpu_id = -1;
    cpu_state->owner = -1;
    cpu_state->fd_pebs = -1;
    init_perf_event(&cpu_state->leader);
    cpu_state->pebs_sampling_period = PEBS_SAMPLE_PERIOD; /* may be overridden in pact_init */
    cpu_state->pebs_mmap = NULL;
    init_perf_event(&cpu_state->count_leader);
    for (int j = 0; j < CPU_EVENT_COUNT; j++) {
        init_perf_event(&cpu_state->count_events[j]);
        cpu_state->count_slot[j] = -1;
        cpu_state->count_last_value[j] = 0;
    }
    cpu_state->nr_count_events = 0;
    cpu_state->count_last_enabled = 0;
    cpu_state->count_last_running = 0;
    cpu_state->count_valid = false;
    cpu_state->llc_misses = 0;
    cpu_state->cha = -1;
    cpu_state->mlp = 0.0;
}

static void setup_one_cpu_perf(per_cpu_state_t *cs, bool skip, bool use_ldlat)
{
    /* The group leader must exist before setup_pebs_event(). It is a dummy
     * event, except for LDLAT on SPR/EMR, where the LDLAT event with a data
     * source needs a MEM_LOADS_AUX leader. */
    bool aux = use_ldlat && g_pmu_platform.id == PMU_PLATFORM_EMR;
    int leader = aux ? setup_mem_loads_aux_leader(&cs->leader, cs->cpu_id)
                     : setup_dummy_leader_event(&cs->leader, -1, cs->cpu_id);
    if (leader < 0) {
        log_warning("perf_events", "Skipping CPU %d (%s leader failed: CPU offline?)", cs->cpu_id,
                    aux ? "MEM_LOADS_AUX" : "dummy");
        cs->fd_pebs = -1;
        return;
    }

    if (skip) {
        cs->fd_pebs = -1;
    } else {
        int r = setup_pebs_event(cs, -1, cs->cpu_id, use_ldlat);
        if (r < 0) {
            log_warning("perf_events", "PEBS setup failed on CPU %d", cs->cpu_id);
            cs->fd_pebs = -1;
        }
    }
}

static int count_configured_pebs_cpus(pact_context_t *pact, int nr_cpus)
{
    int n = 0;
    for (int cpu = 0; cpu < nr_cpus; cpu++) {
        per_cpu_state_t *cpu_state = &pact->cpu_states[cpu];
        if (cpu_state->fd_pebs >= 0 && cpu_state->pebs_mmap) {
            n++;
        }
    }
    return n;
}

/* Tierce: pair every workload CPU with the CHA of its core, whose TOR
 * group counts only that core's requests (TID filter on). A CPU without a
 * paired CHA, or whose CHA group did not open, stops the run: there is no
 * fallback to an aggregate MLP. */
static int pair_cpu_chas(pact_context_t *pact)
{
    for (int c = 0; c < pact->nr_all_cpus; c++) {
        per_cpu_state_t *cs = &pact->cpu_states[c];
        cs->cha = -1;
        for (int k = 0; k < pact->nr_cha; k++) {
            if (pact->cha_pmus[k].core_id == cs->cpu_id) {
                cs->cha = k;
                break;
            }
        }
        if (cs->cha < 0) {
            log_error("setup_pact_perf_events",
                      "CPU %d has no CHA with a TID filter entry; Tierce mode cannot measure "
                      "its MLP",
                      cs->cpu_id);
            return -1;
        }
        if (pact->cha_pmus[cs->cha].group.fds[0] < 0) {
            log_error("setup_pact_perf_events",
                      "The TOR group of the CHA paired with CPU %d "
                      "did not open",
                      cs->cpu_id);
            return -1;
        }
    }
    return 0;
}

/* PACT: global MLP over every CHA (TID filter off) and per-task LLC-miss
 * groups of every workload. */
static int setup_pact_mode_counters(pact_context_t *pact)
{
    /* The MLP counts slow-tier TOR requests of every core of the socket:
     * all mapped CHAs, TID filter off, one ratio of sums. */
    discover_cha_pmus(pact->cha_pmus, &pact->nr_cha, 0);
    setup_pmu_cha_perf_events(pact->cha_pmus, &pact->nr_cha, false);
    log_info("setup_pact_perf_events", "Discovered %d CHAs", pact->nr_cha);
    for (int k = 0; k < pact->nr_cha; k++) {
        if (pact->cha_pmus[k].group.fds[0] < 0) {
            log_error("setup_pact_perf_events",
                      "The TOR group of CHA %d did not open; PACT mode reads every CHA",
                      pact->cha_pmus[k].cha_id);
            pact->sampling_failed = true;
            return -1;
        }
    }

    for (int cpu = 0; cpu < pact->nr_all_cpus; cpu++) {
        per_cpu_state_t *cpu_state = &pact->cpu_states[cpu];
        setup_one_cpu_perf(cpu_state, /*skip=*/false, /*use_ldlat=*/false);
    }

    for (int w = 0; w < pact->nr_workloads; w++) {
        pact_workload_t *wl = &pact->workloads[w];
        int covered = setup_workload_counting_events(wl);
        if (covered == PACT_WORKLOAD_GONE) {
            log_error("setup_pact_perf_events", "Workload %d (PID %d) has no live thread at start",
                      w, (int)wl->target_pid);
            pact->sampling_failed = true;
            return -1;
        }
        if (covered < 0) {
            log_error("setup_pact_perf_events",
                      "Failed to cover the threads of workload %d (PID %d)", w,
                      (int)wl->target_pid);
            pact->sampling_failed = true;
            return -1;
        }
    }
    return 0;
}

void close_cat_counting_events(pact_context_t *pact)
{
    for (int cpu = 0; cpu < pact->nr_all_cpus; cpu++) {
        if (pact->cpu_states[cpu].owner == CAT_FG_WORKLOAD) {
            close_cpu_extra_events(&pact->cpu_states[cpu]);
        }
    }
}

/* Tierce: per-core MLP from the paired CHAs (TID filter on) and one
 * LLC-miss counting group per workload CPU (pid = -1). */
static int setup_tierce_mode_counters(pact_context_t *pact)
{
    discover_cha_pmus(pact->cha_pmus, &pact->nr_cha, pact->all_cpus_mask);
    setup_pmu_cha_perf_events(pact->cha_pmus, &pact->nr_cha, true);
    log_info("setup_pact_perf_events", "Discovered %d CHAs for %d workload CPUs", pact->nr_cha,
             pact->nr_all_cpus);
    if (pair_cpu_chas(pact) < 0) {
        pact->sampling_failed = true;
        return -1;
    }

    /* The LLC way controller's events join the foreground CPUs' groups. If
     * one of them does not open, the controller's events close on every
     * foreground CPU and its counter signal is unavailable. */
    bool extra_failed = false;
    for (int cpu = 0; cpu < pact->nr_all_cpus; cpu++) {
        per_cpu_state_t *cs = &pact->cpu_states[cpu];
        setup_one_cpu_perf(cs, /*skip=*/false, pact->use_ldlat);
        uint32_t extra = (cs->owner == CAT_FG_WORKLOAD) ? pact->cat.events : 0;
        int opened = setup_cpu_counting_group(cs, extra);
        if (opened < 0) {
            log_error("setup_pact_perf_events", "Failed to open the counting group of CPU %d",
                      cs->cpu_id);
            pact->sampling_failed = true;
            return -1;
        }
        extra_failed |= opened > 0;
    }
    if (extra_failed) {
        close_cat_counting_events(pact);
        log_warning("setup_pact_perf_events", "An LLC way controller event did not open; the "
                                              "controller's counter signal is unavailable");
    }
    return 0;
}

int setup_pact_perf_events(pact_context_t *pact)
{
    int setup = (pact->mode == PACT_MODE_TIERCE) ? setup_tierce_mode_counters(pact)
                                                 : setup_pact_mode_counters(pact);
    if (setup < 0) {
        return -1;
    }

    int configured = count_configured_pebs_cpus(pact, pact->nr_all_cpus);
    if (configured < pact->nr_all_cpus) {
        log_error("setup_pact_perf_events",
                  "PEBS set up on %d of %d workload CPUs. The warnings above name the failed "
                  "CPUs. Check permissions and CPU support.",
                  configured, pact->nr_all_cpus);
        return -1;
    } else {
        log_info("setup_pact_perf_events", "PEBS set up on %d CPUs", configured);
        log_info("setup_pact_perf_events",
                 "PEBS validation will occur when workload generates LLC misses");
    }

    start_pmu_perf_events(pact);
    return pact->sampling_failed ? -1 : 0;
}
