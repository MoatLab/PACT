/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/*
 * logging.c - Optional logging implementation for PACT
 *
 * This file contains the actual logging implementation, only compiled when
 * PACT_ENABLE_LOGGING is defined. When logging is disabled, this file is
 * not needed as logging.h provides inline no-ops.
 *
 * The log is CSV. Each row starts with three common columns: timestamp
 * (TSC ticks), event_type and function (the caller). The payload columns
 * arg1 to arg7 follow. Their number and meaning depend on event_type, and
 * a row leaves out the columns that its event does not use:
 *
 *   pebs_sample      workload, page address, tier, attributed stalls
 *   coro_event       coroutine, event, elapsed (us)
 *   pac_calc         page address, frequency, total samples, stalls,
 *                    attributed stalls, tier, MLP
 *   ring_buffer      operation, ring, count, current size, capacity
 *   workload_info    workload, PID, name, CPUs (name and CPUs quoted)
 *   pmu_stats        workload, slow-tier MLP, slow-tier LLC misses,
 *                    slow-tier events
 *   pac_update       workload, page address, score, tier, bin
 *   migration        workload, operation, page address, from tier, to tier
 *   pebs_aggregator  workload, expected samples, processed samples, perf
 *                    lost events, perf lost samples, events dropped with
 *                    the aggregator full, events dropped with the update
 *                    ring full
 */

#include "logging.h"

#ifdef PACT_ENABLE_LOGGING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "pact.h"

/* rdtsc for timestamp */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Initialize logging subsystem
 * Opens log file and writes header
 */
void init_logging(pact_context_t *pact, const char *filename, const char *format)
{
    char log_filename[256];

    /* Store format */
    strncpy(pact->log_format, format, sizeof(pact->log_format) - 1);
    pact->log_format[sizeof(pact->log_format) - 1] = '\0';

    /* Generate filename if not provided */
    if (filename && strlen(filename) > 0) {
        strncpy(log_filename, filename, sizeof(log_filename) - 1);
        log_filename[sizeof(log_filename) - 1] = '\0';
    } else {
        /* Generate log file name based on timestamp */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(log_filename, sizeof(log_filename), "pact_log_%04d%02d%02d_%02d%02d%02d.%s",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday, tm_info->tm_hour,
                 tm_info->tm_min, tm_info->tm_sec, format);
    }

    pact->log_file = fopen(log_filename, "w");
    if (!pact->log_file) {
        fprintf(stderr, "Warning: Failed to open log file %s\n", log_filename);
        pact->enable_logging = false;
        return;
    }

    /* The common columns, then the payload columns (see the top of this
     * file). */
    if (strcmp(format, "csv") == 0) {
        fprintf(pact->log_file,
                "timestamp,event_type,function,arg1,arg2,arg3,arg4,arg5,arg6,arg7\n");
    }

    fflush(pact->log_file);
}

/*
 * Cleanup logging subsystem
 * Closes log file
 */
void cleanup_logging(pact_context_t *pact)
{
    if (!pact->log_file) {
        return;
    }

    fclose(pact->log_file);
    pact->log_file = NULL;
}

/*
 * Unified event logging function
 * All specialized logging functions call this
 */
void log_event(pact_context_t *pact, const char *event_type, const char *function,
               const char *format, ...)
{
    if (!pact->enable_logging || !pact->log_file) {
        return;
    }

    uint64_t timestamp = rdtsc();
    if (strcmp(pact->log_format, "csv") == 0) {
        fprintf(pact->log_file, "%lu,%s,%s,", timestamp, event_type, function);
        va_list args;
        va_start(args, format);
        vfprintf(pact->log_file, format, args);
        va_end(args);
        fprintf(pact->log_file, "\n");
    }
}

/* Specialized logging functions - wrappers around log_event */

void log_pebs_sample(pact_context_t *pact, const char *function, int workload_id, uint64_t addr,
                     uint8_t tier, uint32_t attributed_stalls)
{
    log_event(pact, "pebs_sample", function, "%d,0x%lx,%d,%u", workload_id, addr, tier,
              attributed_stalls);
}

void log_coro_event(pact_context_t *pact, const char *function, const char *coro_name,
                    const char *event, uint64_t elapsed_cycles)
{
    log_event(pact, "coro_event", function, "%s,%s,%lu", coro_name, event, elapsed_cycles);
}

void log_pac_calculation(pact_context_t *pact, const char *function, uint64_t page_addr,
                         uint32_t frequency, uint64_t total_samples, uint64_t stalls,
                         uint64_t attributed_stalls, uint8_t tier, double mlp)
{
    log_event(pact, "pac_calc", function, "0x%lx,%u,%lu,%lu,%lu,%d,%.2f", page_addr, frequency,
              total_samples, stalls, attributed_stalls, tier, mlp);
}

void log_ring_buffer_op(pact_context_t *pact, const char *function, const char *op,
                        const char *buffer_name, size_t count, size_t current_size, size_t capacity)
{
    log_event(pact, "ring_buffer", function, "%s,%s,%zu,%zu,%zu", op, buffer_name, count,
              current_size, capacity);
}

void log_workload_info(pact_context_t *pact, const char *function, int workload_id, pid_t pid,
                       const char *name, const char *cores)
{
    log_event(pact, "workload_info", function, "%d,%d,\"%s\",\"%s\"", workload_id, pid, name,
              cores);
}

void log_pmu(pact_context_t *pact, const char *function, int workload_id, double slow_tier_mlp,
             uint64_t llc_misses_slow, uint64_t slow_tier_events)
{
    log_event(pact, "pmu_stats", function, "%d,%.2f,%lu,%lu", workload_id, slow_tier_mlp,
              llc_misses_slow, slow_tier_events);
}

void log_pac_update(pact_context_t *pact, const char *function, int workload_id, uint64_t page_addr,
                    uint64_t pac_value, const char *tier_str, int bin_index)
{
    log_event(pact, "pac_update", function, "%d,0x%lx,%lu,%s,%d", workload_id, page_addr, pac_value,
              tier_str, bin_index);
}

void log_migration_event(pact_context_t *pact, const char *function, int workload_id,
                         const char *operation, uint64_t page_addr, int from_tier, int to_tier)
{
    log_event(pact, "migration", function, "%d,%s,0x%lx,%d,%d", workload_id, operation, page_addr,
              from_tier, to_tier);
}

void log_pebs_aggregator(pact_context_t *pact, const char *function, int workload_id,
                         uint64_t theory_samples, uint64_t processed_samples,
                         uint64_t perf_dropped_events, uint64_t perf_dropped_samples,
                         uint64_t agg_dropped_events_agg_full,
                         uint64_t agg_dropped_events_update_full)
{
    log_event(pact, "pebs_aggregator", function, "%d,%lu,%lu,%lu,%lu,%lu,%lu", workload_id,
              theory_samples, processed_samples, perf_dropped_events, perf_dropped_samples,
              agg_dropped_events_agg_full, agg_dropped_events_update_full);
}

#endif /* PACT_ENABLE_LOGGING */
