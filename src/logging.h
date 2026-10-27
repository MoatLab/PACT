/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/*
 * logging.h - Optional logging infrastructure for PACT
 *
 * This header provides compile-time and runtime control over logging to minimize
 * performance impact. When PACT_ENABLE_LOGGING is not defined, all logging functions
 * become inline no-ops that the compiler can completely eliminate.
 *
 * Usage:
 *   make            # Logging disabled (zero overhead); the default build
 *                   # excludes logging.c, so -l/--logging is a no-op.
 *   make logging    # Rebuild with PACT_ENABLE_LOGGING; -l/--logging works.
 */

#ifndef PACT_LOGGING_H
#define PACT_LOGGING_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h> /* For pid_t */

/* Forward declaration */
typedef struct pact_context pact_context_t;

/* Mark a no-op stub's parameters as deliberately unused. Guarded so it
 * coexists with the identical definition in pact.h regardless of include
 * order. */
#ifndef PACT_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define PACT_UNUSED __attribute__((unused))
#else
#define PACT_UNUSED
#endif
#endif

/*
 * Compile-time logging control
 * Define PACT_ENABLE_LOGGING to enable logging infrastructure
 * If not defined, all logging functions become zero-cost no-ops
 */
#ifdef PACT_ENABLE_LOGGING

/* Logging enabled - declare actual functions */
void init_logging(pact_context_t *pact, const char *filename, const char *format);
void cleanup_logging(pact_context_t *pact);
void log_event(pact_context_t *pact, const char *event_type, const char *function,
               const char *format, ...);

/* Specialized logging functions */
void log_pebs_sample(pact_context_t *pact, const char *function, int workload_id, uint64_t addr,
                     uint8_t tier, uint32_t attributed_stalls);
void log_coro_event(pact_context_t *pact, const char *function, const char *coro_name,
                    const char *event, uint64_t elapsed_cycles);
void log_pac_calculation(pact_context_t *pact, const char *function, uint64_t page_addr,
                         uint32_t frequency, uint64_t total_samples, uint64_t stalls,
                         uint64_t attributed_stalls, uint8_t tier, double mlp);
void log_ring_buffer_op(pact_context_t *pact, const char *function, const char *op,
                        const char *buffer_name, size_t count, size_t current_size,
                        size_t capacity);
void log_workload_info(pact_context_t *pact, const char *function, int workload_id, pid_t pid,
                       const char *name, const char *cores);
void log_pmu(pact_context_t *pact, const char *function, int workload_id, double slow_tier_mlp,
             uint64_t llc_misses_slow, uint64_t slow_tier_events);
void log_pac_update(pact_context_t *pact, const char *function, int workload_id, uint64_t page_addr,
                    uint64_t pac_value, const char *tier_str, int bin_index);
void log_migration_event(pact_context_t *pact, const char *function, int workload_id,
                         const char *operation, uint64_t page_addr, int from_tier, int to_tier);
void log_pebs_aggregator(pact_context_t *pact, const char *function, int workload_id,
                         uint64_t theory_samples, uint64_t processed_samples,
                         uint64_t perf_dropped_events, uint64_t perf_dropped_samples,
                         uint64_t agg_dropped_events_agg_full,
                         uint64_t agg_dropped_events_update_full);

#else /* PACT_ENABLE_LOGGING not defined */

/* Logging disabled - all functions become inline no-ops. Parameters carry
 * PACT_UNUSED so the empty bodies need no per-parameter (void) casts, while
 * the signatures still type-check every call and keep arguments "used" (so
 * variables passed only to log_*() do not draw unused-variable warnings). */
static inline void init_logging(pact_context_t *pact PACT_UNUSED, const char *filename PACT_UNUSED,
                                const char *format PACT_UNUSED)
{}

static inline void cleanup_logging(pact_context_t *pact PACT_UNUSED) {}

static inline void log_event(pact_context_t *pact PACT_UNUSED, const char *event_type PACT_UNUSED,
                             const char *function PACT_UNUSED, const char *format PACT_UNUSED, ...)
{}

static inline void log_pebs_sample(pact_context_t *pact PACT_UNUSED,
                                   const char *function PACT_UNUSED, int workload_id PACT_UNUSED,
                                   uint64_t addr PACT_UNUSED, uint8_t tier PACT_UNUSED,
                                   uint32_t attributed_stalls PACT_UNUSED)
{}

static inline void log_coro_event(pact_context_t *pact PACT_UNUSED,
                                  const char *function PACT_UNUSED,
                                  const char *coro_name PACT_UNUSED, const char *event PACT_UNUSED,
                                  uint64_t elapsed_cycles PACT_UNUSED)
{}

static inline void
log_pac_calculation(pact_context_t *pact PACT_UNUSED, const char *function PACT_UNUSED,
                    uint64_t page_addr PACT_UNUSED, uint32_t frequency PACT_UNUSED,
                    uint64_t total_samples PACT_UNUSED, uint64_t stalls PACT_UNUSED,
                    uint64_t attributed_stalls PACT_UNUSED, uint8_t tier PACT_UNUSED,
                    double mlp PACT_UNUSED)
{}

static inline void log_ring_buffer_op(pact_context_t *pact PACT_UNUSED,
                                      const char *function PACT_UNUSED, const char *op PACT_UNUSED,
                                      const char *buffer_name PACT_UNUSED, size_t count PACT_UNUSED,
                                      size_t current_size PACT_UNUSED, size_t capacity PACT_UNUSED)
{}

static inline void log_workload_info(pact_context_t *pact PACT_UNUSED,
                                     const char *function PACT_UNUSED, int workload_id PACT_UNUSED,
                                     pid_t pid PACT_UNUSED, const char *name PACT_UNUSED,
                                     const char *cores PACT_UNUSED)
{}

static inline void log_pmu(pact_context_t *pact PACT_UNUSED, const char *function PACT_UNUSED,
                           int workload_id PACT_UNUSED, double slow_tier_mlp PACT_UNUSED,
                           uint64_t llc_misses_slow PACT_UNUSED,
                           uint64_t slow_tier_events PACT_UNUSED)
{}

static inline void log_pac_update(pact_context_t *pact PACT_UNUSED,
                                  const char *function PACT_UNUSED, int workload_id PACT_UNUSED,
                                  uint64_t page_addr PACT_UNUSED, uint64_t pac_value PACT_UNUSED,
                                  const char *tier_str PACT_UNUSED, int bin_index PACT_UNUSED)
{}

static inline void log_migration_event(pact_context_t *pact PACT_UNUSED,
                                       const char *function PACT_UNUSED,
                                       int workload_id PACT_UNUSED,
                                       const char *operation PACT_UNUSED,
                                       uint64_t page_addr PACT_UNUSED, int from_tier PACT_UNUSED,
                                       int to_tier PACT_UNUSED)
{}

static inline void log_pebs_aggregator(
    pact_context_t *pact PACT_UNUSED, const char *function PACT_UNUSED, int workload_id PACT_UNUSED,
    uint64_t theory_samples PACT_UNUSED, uint64_t processed_samples PACT_UNUSED,
    uint64_t perf_dropped_events PACT_UNUSED, uint64_t perf_dropped_samples PACT_UNUSED,
    uint64_t agg_dropped_events_agg_full PACT_UNUSED,
    uint64_t agg_dropped_events_update_full PACT_UNUSED)
{}

#endif /* PACT_ENABLE_LOGGING */

#endif /* PACT_LOGGING_H */
