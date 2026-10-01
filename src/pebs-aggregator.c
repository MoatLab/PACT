/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* PEBS aggregator - aggregates per-CPU PEBS samples into per-page PAC. */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h> /* For PAGE_SIZE */
#include <signal.h> /* kill(pid, 0) workload-exit probe */
#include <time.h>
#include "constants.h"
#include "pact.h"
#include "pmu.h"
#include "error.h"
#include "pebs-aggregator.h"
#include "logging.h"
#include "minicoro.h"
#include "tsc.h"
#include "utils.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE sysconf(_SC_PAGESIZE)
#endif

/* PEBS Aggregator Context */
typedef struct pebs_aggregator {
    /* Per-CPU PEBS contexts from pact.c */
    per_cpu_state_t **cpu_states;
    int num_cpus;
    uint64_t cpu_mask;

    /* Statistics */
    uint64_t events_per_cpu[64]; /* Support up to 64 CPUs */
    uint64_t events_per_tier[2]; /* Per-tier PEBS events (workload-scoped) */
    uint64_t read_events_from_perf;
    uint64_t pushed_events_to_update;
    uint64_t dropped_events_update_full;
    uint64_t dropped_events_workload;
    uint64_t lost_samples;
    uint64_t lost_events;

    /* Round-robin state for fairness */
    int last_cpu_checked;

    /* Back-reference to pact context — used to resolve TGID → workload index
     * at sample-decode time (replaces the old pidmap khash). */
    pact_context_t *pact_ctx;

    /* Always-on diagnostic counters (cumulative across run) */
    uint64_t aggregation_cycles_total;      /* Total aggregation coroutine invocations */
    uint64_t aggregation_max_samples_cycle; /* High-water mark: max samples in one cycle */

    /* Per-cycle sample staging: every CPU's perf buffer is drained here
     * FIRST so the window's total sample count A_t is known before the
     * per-sample stall scalar is computed (Algorithm 1 attributes with
     * same-window counts). Sized to the PAC update ring capacity. */
    uint64_t *cycle_buf;
    int cycle_buf_cap;
} pebs_aggregator_t;

/* Initialize PEBS aggregator */
pebs_aggregator_t *pebs_aggregator_create(per_cpu_state_t *cpu_states, int num_cpus,
                                          uint64_t cpu_mask)
{
    pebs_aggregator_t *agg = calloc(1, sizeof(pebs_aggregator_t));
    if (!agg) {
        return NULL;
    }

    agg->cpu_states = malloc(num_cpus * sizeof(per_cpu_state_t *));
    if (!agg->cpu_states) {
        free(agg);
        return NULL;
    }

    /* Store pointers to CPU states */
    for (int i = 0; i < num_cpus; i++) {
        agg->cpu_states[i] = &cpu_states[i];
    }

    agg->num_cpus = num_cpus;
    agg->cpu_mask = cpu_mask;
    agg->last_cpu_checked = 0;

    agg->cycle_buf_cap = 131072; /* == PAC update ring capacity */
    agg->cycle_buf = malloc((size_t)agg->cycle_buf_cap * sizeof(uint64_t));
    if (!agg->cycle_buf) {
        free(agg->cpu_states);
        free(agg);
        return NULL;
    }

    log_info("pebs_aggregator_create", "Created PEBS aggregator: %d CPUs, mask=0x%lx", num_cpus,
             cpu_mask);

    return agg;
}

/*
 * Copy `len` bytes out of the perf ring buffer starting at byte offset `off`
 * (modulo data_size), reassembling a record that wraps the buffer end. `base`
 * points at the first data page; `data_size` is the data-region size.
 */
static inline void ring_copy(void *dst, const char *base, uint64_t off, size_t len,
                             uint64_t data_size)
{
    uint64_t start = off % data_size;
    size_t first = (start + len <= data_size) ? len : (size_t)(data_size - start);
    memcpy(dst, base + start, first);
    if (first < len) {
        memcpy((char *)dst + first, base, len - first);
    }
}

/* Read PEBS events from one CPU. REMOTE_DRAM only. */
static int read_cpu_pebs_events(pebs_aggregator_t *agg, per_cpu_state_t *cpu_state,
                                uint64_t *events, int max_events)
{
    if (!cpu_state || cpu_state->fd_pebs < 0 || !cpu_state->pebs_mmap) {
        return 0;
    }

    struct perf_event_mmap_page *perf_page = (struct perf_event_mmap_page *)cpu_state->pebs_mmap;

    if (!perf_page) {
        return 0;
    }

    /*
     * perf ring-buffer consumer protocol (see tools/perf and
     * Documentation/userspace-api/perf_ring_buffer.rst):
     *   - data_head / data_tail are free-running ABSOLUTE byte offsets that
     *     increase monotonically; they are NOT reduced modulo the buffer size.
     *     Available bytes = data_head - data_tail (unsigned arithmetic).
     *   - The buffer holds PERF_BUFFER_PAGES pages; index into it with
     *     (offset % data_size). The mmap is sized (1 + PERF_BUFFER_PAGES)
     *     pages in setup_pebs_event(), so this constant is authoritative.
     *   - Read head, then issue an rmb() BEFORE reading the records it
     *     published, so record payloads (written before head advanced) are
     *     visible. Issue an mb() before writing data_tail back.
     */
    const uint64_t data_size = (uint64_t)PERF_BUFFER_PAGES * PAGE_SIZE;
    char *data = (char *)cpu_state->pebs_mmap + PAGE_SIZE; /* records follow the metadata page */

    uint64_t head = perf_page->data_head;
    uint64_t tail = perf_page->data_tail;

    /* Acquire: see record payloads published before this head value. */
    __sync_synchronize();

    if (head == tail) {
        return 0; /* No new data */
    }

    /*
     * The buffer is a single (non-mirrored) mapping, so a record can wrap the
     * end of the buffer. Reassemble each record into a small linear scratch
     * buffer before reading its fields, so a wrapping record is handled
     * correctly instead of reading past the mapping. Our records are tiny
     * (header + TID + ADDR), so a fixed scratch is sufficient.
     */
    int count = 0;
    while (tail < head && count < max_events) {
        uint64_t off = tail % data_size;

        struct perf_event_header hdr;
        ring_copy(&hdr, data, off, sizeof(hdr), data_size);
        if (hdr.size < sizeof(hdr)) {
            break; /* malformed/zero-size header: stop to avoid an infinite loop */
        }

        if (hdr.type == PERF_RECORD_SAMPLE) {
            /*
             * Sample layout (ordered by sample_type bit position):
             *   PERF_SAMPLE_TID:      { u32 pid, tid; }
             *   PERF_SAMPLE_ADDR:     { u64 addr; }
             */
            struct {
                struct perf_event_header header;
                uint32_t pid;
                uint32_t tid;
                uint64_t addr;
            } rec;
            if (hdr.size >= sizeof(rec)) {
                ring_copy(&rec, data, off, sizeof(rec), data_size);
                if (is_target_pid(agg->pact_ctx, (pid_t)rec.pid) &&
                    agg->pact_ctx->workload->counters_valid &&
                    workload_covers_tid(agg->pact_ctx->workload, (pid_t)rec.tid)) {
                    events[count++] = PEBS_ENCODE_ADDR_TIER(rec.addr, 1);
                    agg->events_per_tier[1]++;
                }
            }
        }

        /* Advance by the raw record size (absolute counter, no modulo). */
        tail += hdr.size;
    }

    /* Release: publish the consumed position only after reading the records. */
    __sync_synchronize();
    perf_page->data_tail = tail;

    return count;
}

/* Aggregate PEBS events from all CPUs and push directly to PAC update ring */
/* Per-sample stall attribution = the cycle's per-tier scalar.
 * PEBS_ENCODE_PAC packs the PAC value into 18 bits, so clamp to 262143 here:
 * a larger value would be silently truncated by the encoder and corrupt the
 * PAC score. */
#define PAC_VALUE_MAX 262143u
static inline uint32_t compute_sample_stalls(uint8_t tier, double fast_stalls, double slow_stalls)
{
    double base = (tier == 0) ? fast_stalls : slow_stalls;
    if (base < 0.0) {
        return 0;
    }
    if (base > (double)PAC_VALUE_MAX) {
        return PAC_VALUE_MAX;
    }
    return (uint32_t)base;
}

/* Decode one PEBS sample into address/tier. */
static inline void decode_pebs_sample(const uint64_t *events, int i, uint64_t *out_addr,
                                      uint8_t *out_tier)
{
    uint64_t encoded = events[i];
    *out_addr = PEBS_DECODE_ADDR(encoded);
    *out_tier = PEBS_DECODE_TIER(encoded);
}

/* Per-workload attributed stalls = k_constant * llc_misses / (mlp * events).
 * The /pebs_sampling_period division is intentionally absent: LDLAT filtering,
 * ring drops, and quiet-skip losses break the ideal ev≈miss/period identity,
 * so we let PAC scale with observed sample population instead. */
static void compute_attributed_stalls(pact_context_t *ctx, pebs_aggregator_t *agg,
                                      double *out_fast_stalls, double *out_slow_stalls)
{
    *out_fast_stalls = 0.0;
    *out_slow_stalls = 0.0;
    pact_workload_t *wl = ctx->workload;
    double fast_coeff = wl->workload_mlp_fast * agg->events_per_tier[0];
    double slow_coeff = wl->workload_mlp_slow * agg->events_per_tier[1];
    if (fast_coeff > 0) {
        *out_fast_stalls = (ctx->k_constant_dram * wl->stats.llc_misses_fast) / fast_coeff;
    }
    if (slow_coeff > 0) {
        *out_slow_stalls = (ctx->k_constant_cxl * wl->stats.llc_misses_slow) / slow_coeff;
    }
}

/*
 * Drain, attribute, and publish one window's PEBS samples.
 *
 * Phase A drains every CPU's perf buffer into cycle_buf so the window's
 * per-tier sample population is known; phase B then computes the per-tier
 * stall scalar from THIS window's counters and counts (Algorithm 1: S_p =
 * S * A_p / A_t with all terms from the same window) and pushes each
 * attributed sample to the PAC update ring.
 */
int pebs_aggregate_events(pebs_aggregator_t *agg, pact_context_t *ctx)
{
    if (!agg || !ctx) {
        return -1;
    }

    /* Phase A: drain all CPUs into the staging buffer. Round-robin start
     * keeps CPU 0 from monopolizing the buffer when it fills. */
    int n = 0;
    for (int ci = 0; ci < agg->num_cpus; ci++) {
        int cpu = (agg->last_cpu_checked + ci) % agg->num_cpus;
        per_cpu_state_t *cpu_state = agg->cpu_states[cpu];

        int count;
        do {
            int room = agg->cycle_buf_cap - n;
            if (room == 0) {
                break;
            }
            count =
                read_cpu_pebs_events(agg, cpu_state, agg->cycle_buf + n, room < 2048 ? room : 2048);
            agg->events_per_cpu[cpu] += count;
            agg->read_events_from_perf += count;
            n += count;
        } while (count > 0);

        /* Staging full: exhaust this CPU's perf buffer into drop counters
         * so the producer ring cannot wedge. */
        if (n == agg->cycle_buf_cap) {
            uint64_t scratch[512];
            int dropped;
            while ((dropped = read_cpu_pebs_events(agg, cpu_state, scratch, 512)) > 0) {
                agg->read_events_from_perf += dropped;
                agg->dropped_events_update_full += dropped;
                agg->dropped_events_workload += dropped;
            }
        }
    }
    agg->last_cpu_checked = (agg->last_cpu_checked + 1) % agg->num_cpus;

    /* Phase B: same-window attribution. events_per_tier was populated by
     * the phase-A reads; the LLC-miss and MLP inputs were read from the
     * PMU for this same window just before this call. */
    double fast_stalls = 0.0, slow_stalls = 0.0;
    compute_attributed_stalls(ctx, agg, &fast_stalls, &slow_stalls);

    int pac_count = 0;
    for (int i = 0; i < n; i++) {
        uint64_t addr;
        uint8_t tier;
        decode_pebs_sample(agg->cycle_buf, i, &addr, &tier);
        uint64_t page = addr & PAGE_MASK;
        uint32_t attributed = compute_sample_stalls(tier, fast_stalls, slow_stalls);
        log_pebs_sample(ctx, "pebs_aggregate_events", 0, addr, tier, attributed);

        uint64_t pac_encoded = PEBS_ENCODE_PAC(attributed, page, tier);
        if (ring_buffer_uint64_push(ctx->pac_update_ring, pac_encoded) == 0) {
            /* Ring full; the adaptive coroutine drains it on this same
             * thread, so nothing frees up mid-loop. Drop the rest. */
            agg->dropped_events_update_full += (uint64_t)(n - i);
            break;
        }
        pac_count++;
    }

    agg->pushed_events_to_update += pac_count;

    agg->aggregation_cycles_total++;
    if ((uint64_t)pac_count > agg->aggregation_max_samples_cycle) {
        agg->aggregation_max_samples_cycle = pac_count;
    }

    return pac_count;
}

/* Reset per-cycle drop/lost counters at the top of each aggregation
 * iteration. */
static void reset_per_cycle_counters(pebs_aggregator_t *agg)
{
    agg->read_events_from_perf = 0;
    agg->pushed_events_to_update = 0;
    agg->dropped_events_update_full = 0;
    agg->dropped_events_workload = 0;
    agg->lost_samples = 0;
    agg->lost_events = 0;
}

/* Check workload liveness independently of per-window thread discovery. */
static bool poll_and_update_threads(pact_context_t *ctx, int debug_counter)
{
    (void)debug_counter;
    return pact_check_all_targets_exited(ctx);
}

static void log_workload_pebs_stats(pact_context_t *ctx, pebs_aggregator_t *agg)
{
    pact_workload_t *wl = ctx->workload;
    uint64_t theory =
        (wl->stats.llc_misses_fast + wl->stats.llc_misses_slow) / ctx->pebs_sampling_period;
    uint64_t processed = agg->events_per_tier[0] + agg->events_per_tier[1];
    log_pebs_aggregator(ctx, "pebs_aggregator_coroutine", 0, theory, processed, agg->lost_events,
                        agg->lost_samples, 0, agg->dropped_events_workload);
}

/* Modified PEBS coroutine that uses aggregator */
void pebs_aggregator_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);

    /* Get aggregator from context */
    pebs_aggregator_t *agg = ctx->pebs_aggregator;
    if (!agg) {
        log_error("pebs_aggregator_coroutine", "No aggregator context available");
        return;
    }

    log_info("pebs_aggregator_coroutine", "Starting with %d CPUs, mask=0x%lx", agg->num_cpus,
             agg->cpu_mask);

    int debug_counter = 0;

    while (ctx->running) {
        reset_per_cycle_counters(agg);
        /* Fresh per-tier sample counts for this window; phase A of
         * pebs_aggregate_events repopulates them from the drained samples
         * so attribution divides by this window's population. */
        agg->events_per_tier[0] = 0;
        agg->events_per_tier[1] = 0;

        /* turn off pmu counters */
        stop_pmu_perf_events(ctx);
        if (ctx->sampling_failed) {
            break;
        }

        /* read counting events, e.g. LLC stalls, CHA stats */
        read_pmu_counting_events(ctx);

        if (poll_and_update_threads(ctx, debug_counter)) {
            log_info("pebs_aggregator_coroutine", "Workload has exited, stopping PACT");
            ctx->running = false;
            mco_yield(co);
            break;
        }

        /* Keep the old task population and counters stopped until its samples
         * have been attributed. Newly created tasks join the next window. */
        int aggregated = pebs_aggregate_events(agg, ctx);
        if (setup_workload_counting_events(ctx->workload) < 0) {
            if (pact_check_all_targets_exited(ctx)) {
                ctx->running = false;
                break;
            }
            log_error("pebs_aggregator_coroutine", "Failed to refresh task counter coverage");
            ctx->sampling_failed = true;
            ctx->running = false;
            break;
        }
        start_pmu_perf_events(ctx);
        ctx->timing[CORO_TYPE_PEBS].next_tsc = rdtsc() + ms_to_tsc(ctx, ctx->sampling_interval_ms);

        /* Accumulate per-workload PEBS samples into cumulative counter */
        ctx->workload->total_pebs_samples += agg->events_per_tier[0] + agg->events_per_tier[1];

        /* Log workload PMU stats (after aggregation so event counts are available) */
        log_pmu(ctx, "pebs_aggregator_coroutine", 0, ctx->workload->workload_mlp_fast,
                ctx->workload->workload_mlp_slow, ctx->workload->stats.llc_misses_fast,
                ctx->workload->stats.llc_misses_slow, agg->events_per_tier[0],
                agg->events_per_tier[1]);

        if (aggregated == 0) {
            /* No events available - yield */
            /* Debug: Print periodically */
            if (++debug_counter % 1000 == 0) {
                log_debug("pebs_aggregator_coroutine",
                          "PEBS aggregator: No events collected (attempt %d)", debug_counter);
            }
            mco_yield(co);
            continue;
        } else {
            ctx->sample_counts += aggregated;
            ctx->workload->stats.pebs_events_processed += aggregated;
            log_debug("pebs_aggregator_coroutine", "PEBS aggregator: Pushed %d events to PAC ring",
                      aggregated);
            debug_counter = 0;
        }

        log_workload_pebs_stats(ctx, agg);

        /* Mark PAC coroutine ready if the ring has data to drain. */
        if (ring_buffer_uint64_size(ctx->pac_update_ring) > 0) {
        }

        mco_yield(co);
    }

    log_info("pebs_aggregator_coroutine", "PEBS Aggregator: Exiting");
}

/* Setup PEBS aggregator for unified context */
int setup_pebs_aggregator(pact_context_t *ctx, per_cpu_state_t *cpu_states, int num_cpus,
                          uint64_t cpu_mask)
{
    /* Create aggregator using per-CPU states */
    pebs_aggregator_t *agg = pebs_aggregator_create(cpu_states, num_cpus, cpu_mask);

    if (!agg) {
        log_error("setup_pebs_aggregator", "Failed to create PEBS aggregator");
        return -1;
    }

    /* Store back-reference to pact context (needed for CPU-to-workload mapping) */
    agg->pact_ctx = ctx;

    /* Store in context */
    ctx->pebs_aggregator = agg;

    /* Mark PEBS as available via aggregator */
    ctx->pebs_available = true;

    log_info("setup_pebs_aggregator", "PEBS filtering enabled for PID %d",
             ctx->workload->target_pid);
    log_info("setup_pebs_aggregator", "PEBS aggregator setup complete");
    return 0;
}

/* Cleanup aggregator */
void pebs_aggregator_destroy(pebs_aggregator_t *agg)
{
    if (!agg) {
        return;
    }

    log_info("pebs_aggregator_destroy", "Destroying PEBS aggregator");

    free(agg->cycle_buf);
    free(agg->cpu_states);
    free(agg);
}
