/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* PEBS aggregator - interface. */

#ifndef PEBS_AGGREGATOR_H
#define PEBS_AGGREGATOR_H

#include <stdint.h>
#include "pact.h"
#include "minicoro.h"

/* Forward declarations */
typedef struct pebs_aggregator pebs_aggregator_t;
typedef struct per_cpu_state per_cpu_state_t;

/* Function declarations */

/*
 * Create PEBS aggregator
 * @cpu_states: Array of per-CPU PEBS states
 * @num_cpus: Number of CPUs
 * @cpu_mask: Bitmask of active CPUs
 * @return: Aggregator context or NULL on failure
 */
pebs_aggregator_t *pebs_aggregator_create(per_cpu_state_t *cpu_states, int num_cpus,
                                          uint64_t cpu_mask);

/*
 * Aggregate PEBS events from all CPUs and push directly to PAC update ring
 * @agg: Aggregator context
 * @ctx: PACT context (for pac_update_ring)
 * @return: Number of events pushed to pac_update_ring, or -1 on error
 */
int pebs_aggregate_events(pebs_aggregator_t *agg, pact_context_t *ctx);

/*
 * PEBS aggregator coroutine: one sampling window per tick, all CPUs
 */
void pebs_aggregator_coroutine(mco_coro *co);

/*
 * Setup PEBS aggregator for the PACT context
 * @ctx: PACT context to initialize
 * @cpu_states: Array of per-CPU PEBS states
 * @num_cpus: Number of CPUs
 * @cpu_mask: Bitmask of active CPUs
 * @return: 0 on success, -1 on failure
 */
int setup_pebs_aggregator(pact_context_t *ctx, per_cpu_state_t *cpu_states, int num_cpus,
                          uint64_t cpu_mask);

/*
 * Cleanup aggregator resources
 * @agg: Aggregator to destroy
 */
void pebs_aggregator_destroy(pebs_aggregator_t *agg);

#endif /* PEBS_AGGREGATOR_H */
