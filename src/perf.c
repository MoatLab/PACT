/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* perf.c — perf event setup at init time.
 *
 * setup_* helpers (setup_counting_event, setup_pebs_event,
 * setup_pmu_cha_perf_events, setup_workload_counting_events,
 * start_pmu_perf_events, discover_cha_pmus) live in pmu.c — included via pmu.h.
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
    cpu_state->fd_pebs = -1;
    init_perf_event(&cpu_state->leader);
    cpu_state->pebs_sampling_period = PEBS_SAMPLE_PERIOD; /* may be overridden in pact_init */
    cpu_state->pebs_mmap = NULL;
}

static void setup_one_cpu_perf(per_cpu_state_t *cs, bool skip)
{
    /* Dummy leader owns the perf mmap that sampling events attach to —
     * must be created before setup_pebs_event(). */
    if (setup_dummy_leader_event(&cs->leader, -1, cs->cpu_id) < 0) {
        log_warning("perf_events", "Skipping CPU %d (dummy leader failed — CPU offline?)",
                    cs->cpu_id);
        cs->fd_pebs = -1;
        return;
    }

    if (skip) {
        cs->fd_pebs = -1;
    } else {
        int r = setup_pebs_event(cs, -1, cs->cpu_id);
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

int setup_pact_perf_events(pact_context_t *pact)
{
    pact_workload_t *wl = pact->workload;

    /* Discover and configure CHA PMUs for the workload. */
    discover_cha_pmus(wl->cha_pmus, &wl->nr_cha, wl->cpu_mask);
    setup_pmu_cha_perf_events(wl->cha_pmus, &wl->nr_cha);
    log_info("setup_pact_perf_events", "Workload (PID %d): Discovered %d CHAs", wl->target_pid,
             wl->nr_cha);

    for (int cpu = 0; cpu < pact->nr_all_cpus; cpu++) {
        per_cpu_state_t *cpu_state = &pact->cpu_states[cpu];
        setup_one_cpu_perf(cpu_state, /*skip=*/false);
    }

    if (setup_workload_counting_events(wl) < 0) {
        log_error("setup_pact_perf_events", "Failed to cover workload threads");
        pact->sampling_failed = true;
        return -1;
    }

    int configured = count_configured_pebs_cpus(pact, pact->nr_all_cpus);
    if (configured == 0) {
        log_error("setup_pact_perf_events",
                  "PEBS not set up on any CPU! Check permissions and CPU support.");
        return -1;
    } else {
        log_info("setup_pact_perf_events", "PEBS set up on %d CPUs", configured);
        log_info("setup_pact_perf_events",
                 "PEBS validation will occur when workload generates LLC misses");
    }

    start_pmu_perf_events(pact);
    return pact->sampling_failed ? -1 : 0;
}
