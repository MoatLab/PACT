/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* perf.h — perf event setup at init time. */

#ifndef PACT_SETUP_PERF_H
#define PACT_SETUP_PERF_H

#include "pact.h"

/* Open per-CPU PEBS events, the workload's per-task counting groups,
 * and the workload's CHAs. Called once during init, after the PEBS
 * aggregator is set up. Return -1 if no PEBS CPU can be configured. */
int setup_pact_perf_events(pact_context_t *pact);

/* Zero/sentinel-initialize a single perf_event_t (fd=-1, id=-1, counters=0). */
void init_perf_event(perf_event_t *perf_event);

/* Initialize a per-CPU state struct (dummy leader + PEBS fd/mmap slots). */
void init_per_cpu_state(per_cpu_state_t *cpu_state);

#endif /* PACT_SETUP_PERF_H */
