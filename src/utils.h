/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* utils.h: small stateless utility functions. */

#ifndef PACT_UTILS_H
#define PACT_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h> /* getpid */

#include "pact.h" /* fast_prng_t */
#include "tsc.h"  /* rdtsc */

/* Relaxed-memory-order atomic counter primitives.
 *
 * Used for cross-thread statistics counters where the only requirement is
 * "don't tear / don't lose updates": no ordering or release/acquire
 * pairing with other state. Relaxed is the right choice and yields one
 * locked instruction on x86 (`lock xadd`).
 *
 * Type-generic macros (function-like): work for any integer pointer the
 * compiler's __atomic_* intrinsics accept (uint32_t / uint64_t / size_t).
 * Parens around `p` / `n` as the standard macro-safety idiom. */
#define atomic_inc_relaxed(p) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)
#define atomic_add_relaxed(p, n) __atomic_add_fetch((p), (n), __ATOMIC_RELAXED)

/* Fast thread-safe PRNG (xorshift64).
 * Inline for hot-path use (called from PEBS sample processing). */
static inline uint64_t fast_prng_next(fast_prng_t *prng)
{
    uint64_t x = prng->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng->state = x;
    return x;
}

static inline void fast_prng_init(fast_prng_t *prng)
{
    /* PID + TSC seeding (single-threaded, no pthread). */
    prng->state = ((uint64_t)getpid() << 32) ^ rdtsc();
    if (prng->state == 0) {
        prng->state = 1; /* avoid zero state */
    }
}

/* Mark a workload exited, once: it is no longer sampled. Only a pidfd that
 * reports the exit leads here (pact_workload_exited). A counter refresh
 * that finds no live thread does not mark the workload exited. */
void pact_mark_workload_exited(pact_workload_t *wl);

/* Poll one workload's pidfd and mark the workload exited once its process
 * has terminated. A pidfd error stops sampling. Returns wl->exited. */
bool pact_workload_exited(pact_context_t *ctx, pact_workload_t *wl);

/* Check if all workload target PIDs have exited.
 * Returns true only if at least one workload is configured AND all have died.
 * Each workload's pidfd reports termination even before the parent reaps it. */
bool pact_check_all_targets_exited(pact_context_t *ctx);

/* Parse a CPU list such as "2-5,8" into a mask of CPUs 0-63. Returns 0, or
 * -1 for a syntax error, an empty list, or a CPU above 63. */
int pact_parse_cpu_list(const char *text, uint64_t *mask);

/* TSC frequency detection: sleeps 10ms and measures TSC delta against
 * CLOCK_MONOTONIC. Returns Hz. Used once at init. */
uint64_t pact_detect_tsc_frequency(void);

/* Pin the calling thread to a specific CPU via sched_setaffinity.
 * Returns 0 on success, or -1 with errno set on failure (logged via
 * log_error). */
int pact_pin_to_cpu(int cpu);

#endif /* PACT_UTILS_H */
