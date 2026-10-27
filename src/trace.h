/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* trace.h: the per-second migration trace (--trace-migration FILE).
 *
 * One CSV row per second: the run's promotions, the runtime demotions
 * (0: the runtime only promotes and the kernel demotes) and the kernel
 * demotions of the workload cgroups (pgdemote since start), each as a total
 * and as a rate, then the same per workload, with each workload cgroup's
 * kernel demotions, in both modes. A kernel demotion count that
 * cannot be read repeats its last value: the log shows the first failure,
 * and the close line gives the number of failed reads.
 */

#ifndef PACT_TRACE_H
#define PACT_TRACE_H

#include <stdint.h>
#include <stdio.h>

struct pact_context;

#define MIGRATION_TRACE_MAX_WORKLOADS 16 /* MAX_WORKLOADS (pact.h) */

typedef struct migration_trace {
    FILE *file;
    uint64_t next_tsc; /* event-loop deadline of the next row; 0 = off */
    uint64_t last_tsc;
    uint64_t last_promotions[MIGRATION_TRACE_MAX_WORKLOADS];
    uint64_t last_kernel[MIGRATION_TRACE_MAX_WORKLOADS];
    uint64_t rows;
    uint64_t kernel_read_failures; /* unreadable pgdemote counts (last value kept) */
} migration_trace_t;

/* Open the trace and write its header. Returns 0, or -1 when the file
 * cannot be opened (the run continues without the trace). */
int migration_trace_open(struct pact_context *ctx, const char *path);

/* Write one row for the interval since the previous row. */
void migration_trace_row(struct pact_context *ctx);

/* Close the trace. Safe to call when it is off. */
void migration_trace_close(struct pact_context *ctx);

#endif /* PACT_TRACE_H */
