/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* trace.c: the per-second migration trace (see trace.h). */

#include <errno.h>
#include <string.h>

#include "balance.h" /* read_cgroup_demotions */
#include "error.h"
#include "pact.h"
#include "trace.h"
#include "tsc.h"

_Static_assert(MIGRATION_TRACE_MAX_WORKLOADS == MAX_WORKLOADS, "one trace slot per workload");

/* Kernel demotions of a workload cgroup since the run started. An
 * unreadable count repeats the last value; the first failure is logged. */
static uint64_t kernel_demotions(migration_trace_t *t, const pact_workload_t *wl, uint64_t last)
{
    uint64_t demoted;
    if (read_cgroup_demotions(wl->cgroup_path, &demoted) < 0) {
        if (t->kernel_read_failures++ == 0) {
            log_warning("migration_trace",
                        "Cannot read pgdemote of %s; the trace repeats the last kernel demotion "
                        "count while the read fails",
                        wl->cgroup_path);
        }
        return last;
    }
    return demoted >= wl->demotion_baseline ? demoted - wl->demotion_baseline : 0;
}

int migration_trace_open(pact_context_t *ctx, const char *path)
{
    migration_trace_t *t = &ctx->migration_trace;
    memset(t, 0, sizeof(*t));
    t->file = fopen(path, "w");
    if (!t->file) {
        log_error("migration_trace", "Cannot open the migration trace %s: %s", path,
                  strerror(errno));
        return -1;
    }
    fprintf(t->file, "elapsed_sec,total_pact_promotions,pact_promotion_speed,total_pact_demotions,"
                     "pact_demotion_speed,total_kernel_demotions,kernel_demotion_speed");
    for (int w = 0; w < ctx->nr_workloads; w++) {
        fprintf(t->file,
                ",wl%d_pact_promotions,wl%d_pact_promotion_speed,wl%d_pact_demotions,"
                "wl%d_pact_demotion_speed,wl%d_kernel_demotions,wl%d_kernel_demotion_speed",
                w, w, w, w, w, w);
    }
    fputc('\n', t->file);
    fflush(t->file);
    t->last_tsc = rdtsc();
    t->next_tsc = t->last_tsc + sec_to_tsc(ctx, 1);
    log_info("migration_trace", "Migration trace: %s", path);
    return 0;
}

void migration_trace_row(pact_context_t *ctx)
{
    migration_trace_t *t = &ctx->migration_trace;
    if (!t->file) {
        return;
    }
    uint64_t now = rdtsc();
    double interval = (double)(now - t->last_tsc) / (double)ctx->tsc_freq_hz;
    if (interval < 0.001) {
        return;
    }
    double elapsed = (double)(now - ctx->start_tsc) / (double)ctx->tsc_freq_hz;

    uint64_t promotions[MAX_WORKLOADS], kernel[MAX_WORKLOADS];
    uint64_t sum[2] = {0}, last_sum[2] = {0};
    for (int w = 0; w < ctx->nr_workloads; w++) {
        pact_workload_t *wl = &ctx->workloads[w];
        promotions[w] = atomic_load(&wl->stats.promotion_successes);
        kernel[w] = kernel_demotions(t, wl, t->last_kernel[w]);
        sum[0] += promotions[w];
        sum[1] += kernel[w];
        last_sum[0] += t->last_promotions[w];
        last_sum[1] += t->last_kernel[w];
    }
    double rate[2];
    for (int k = 0; k < 2; k++) {
        uint64_t delta = sum[k] >= last_sum[k] ? sum[k] - last_sum[k] : 0;
        rate[k] = (double)delta / interval;
    }
    /* Runtime demotions: 0. The runtime only promotes and the kernel
     * demotes. */
    fprintf(t->file, "%.6f,%lu,%.2f,0,0.00,%lu,%.2f", elapsed, sum[0], rate[0], sum[1], rate[1]);
    for (int w = 0; w < ctx->nr_workloads; w++) {
        uint64_t delta = kernel[w] >= t->last_kernel[w] ? kernel[w] - t->last_kernel[w] : 0;
        fprintf(t->file, ",%lu,%.2f,0,0.00,%lu,%.2f", promotions[w],
                (double)(promotions[w] - t->last_promotions[w]) / interval, kernel[w],
                (double)delta / interval);
        t->last_promotions[w] = promotions[w];
        t->last_kernel[w] = kernel[w];
    }
    fputc('\n', t->file);
    fflush(t->file);
    t->last_tsc = now;
    t->rows++;
}

void migration_trace_close(pact_context_t *ctx)
{
    migration_trace_t *t = &ctx->migration_trace;
    if (!t->file) {
        return;
    }
    fclose(t->file);
    t->file = NULL;
    t->next_tsc = 0;
    log_info("migration_trace", "Migration trace closed (%lu rows, %lu unreadable pgdemote counts)",
             t->rows, t->kernel_read_failures);
}
