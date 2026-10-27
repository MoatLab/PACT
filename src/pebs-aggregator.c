/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* PEBS aggregator - aggregates per-CPU PEBS samples into per-page PAC. */

#include <stddef.h> /* offsetof */
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

/* One drained sample, held until its window's S_w is known. */
typedef struct staged_sample {
    uint64_t page;     /* Page address */
    uint32_t workload; /* Owner workload number */
    uint32_t latency;  /* LDLAT: load latency in cycles (clamped); else 0 */
} staged_sample_t;

/* PEBS Aggregator Context */
typedef struct pebs_aggregator {
    /* Per-CPU PEBS contexts from pact.c */
    per_cpu_state_t **cpu_states;
    int num_cpus;
    uint64_t cpu_mask;

    /* Statistics */
    uint64_t events_per_cpu[64]; /* Support up to 64 CPUs */
    /* Covered slow-tier samples of this window, per workload and in total
     * (N in the stall scalar). A sample dropped at a full staging buffer
     * still counts. */
    uint64_t samples[MAX_WORKLOADS];
    uint64_t total_samples;
    uint64_t pushed[MAX_WORKLOADS]; /* Samples of this window pushed to the PAC ring */
    uint64_t read_events_from_perf;
    uint64_t pushed_events_to_update;
    uint64_t dropped_events_update_full;
    uint64_t dropped_events_workload;
    uint64_t lost_samples;
    uint64_t lost_events;

    /* LDLAT: latency sum and count of this window's samples per workload
     * that passed the data-source filter, before any software gate (the
     * k_slow input). */
    uint64_t ldlat_latency_sum[MAX_WORKLOADS];
    uint64_t ldlat_latency_count[MAX_WORKLOADS];

    /* Round-robin state for fairness */
    int last_cpu_checked;

    /* Back-reference to pact context: used to resolve TGID -> workload index
     * at sample-decode time. */
    pact_context_t *pact_ctx;

    /* Always-on diagnostic counters (cumulative across run) */
    uint64_t aggregation_cycles_total;      /* Total aggregation coroutine invocations */
    uint64_t aggregation_max_samples_cycle; /* High-water mark: max samples in one cycle */

    /* Per-cycle sample staging: every CPU's perf buffer is drained here
     * FIRST so the window's total sample count A_t is known before the
     * per-sample stall scalar is computed (Algorithm 1 attributes with
     * same-window counts). Sized to the PAC update ring capacity. */
    staged_sample_t *cycle_buf;
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

    agg->cycle_buf_cap = PAC_UPDATE_RING_SIZE;
    agg->cycle_buf = malloc((size_t)agg->cycle_buf_cap * sizeof(*agg->cycle_buf));
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

/* The workload a sample belongs to, or -1 when the sample is dropped. A
 * sample of another process is ignored. PACT: a sample of a thread whose
 * counting group did not cover this window is counted as uncovered.
 * Tierce: a sample belongs to the workload that owns the CPU; it is
 * counted as uncovered when the CPU's counting group did not cover this
 * window. */
static int sample_workload(pebs_aggregator_t *agg, const per_cpu_state_t *cpu_state, pid_t pid,
                           pid_t tid)
{
    pact_context_t *ctx = agg->pact_ctx;
    if (ctx->mode == PACT_MODE_TIERCE) {
        if (cpu_state->owner < 0 || ctx->workloads[cpu_state->owner].target_pid != pid) {
            return -1;
        }
        if (!cpu_state->count_valid) {
            ctx->workloads[cpu_state->owner].stats.pebs_uncovered++;
            return -1;
        }
        return cpu_state->owner;
    }
    int w = pact_find_workload(ctx, pid);
    if (w < 0) {
        return -1;
    }
    pact_workload_t *wl = &ctx->workloads[w];
    if (!workload_covers_tid(wl, tid)) {
        wl->stats.pebs_uncovered++;
        return -1;
    }
    return w;
}

/* perf_mem_data_src of a remote-DRAM (slow-tier) load: the legacy mem_lvl
 * REM_RAM1/REM_RAM2 bits, or the newer encoding (level RAM, remote). Some
 * kernels report Skylake's remote-DRAM data source only in the newer one. */
static inline bool is_remote_ram(uint64_t data_src)
{
    uint64_t mem_lvl = (data_src >> PERF_MEM_LVL_SHIFT) & 0x3FFF;
    uint64_t lvl_num = (data_src >> PERF_MEM_LVLNUM_SHIFT) & 0xF;
    uint64_t remote = (data_src >> PERF_MEM_REMOTE_SHIFT) & 0x1;
    if (mem_lvl & (PERF_MEM_LVL_REM_RAM1 | PERF_MEM_LVL_REM_RAM2)) {
        return true;
    }
    return remote && lvl_num == PERF_MEM_LVLNUM_RAM;
}

/* LDLAT filter of one covered sample, in order: the data-source filter
 * keeps remote-DRAM loads; every kept load feeds the workload's k_slow
 * input; then the software gate drops loads below --ldlat-gate, and the
 * LDLAT-Dyn gate applies when it runs. Returns true when the sample is
 * kept. */
static bool ldlat_keep(pebs_aggregator_t *agg, int w, uint32_t latency, uint64_t data_src)
{
    pact_context_t *ctx = agg->pact_ctx;
    pact_workload_t *wl = &ctx->workloads[w];
    if (!is_remote_ram(data_src)) {
        wl->ldlat_filtered++;
        return false;
    }
    agg->ldlat_latency_sum[w] += latency;
    agg->ldlat_latency_count[w]++;
    if (ctx->ldlat_gate > LDLAT_HW_THRESHOLD && latency < ctx->ldlat_gate) {
        wl->ldlat_gated++;
        return false;
    }
    if (ctx->ldlat_dyn.enabled && !ldlat_dyn_admit(&ctx->ldlat_dyn, latency)) {
        wl->ldlat_gated++;
        return false;
    }
    return true;
}

/* Read PEBS events from one CPU into staged samples: slow-tier LLC-miss
 * loads, or LDLAT loads that pass the LDLAT filter. */
static int read_cpu_pebs_events(pebs_aggregator_t *agg, per_cpu_state_t *cpu_state,
                                staged_sample_t *events, int max_events)
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
     * (header, TID and ADDR, plus WEIGHT and DATA_SRC with LDLAT), so a
     * fixed scratch is sufficient.
     */
    pact_stats_t *quality = &agg->pact_ctx->stats;
    if (head - tail > data_size) {
        quality->pebs_overruns++;
        __sync_synchronize();
        perf_page->data_tail = head;
        return 0;
    }
    int count = 0;
    while (head != tail && count < max_events) {
        if (head - tail < sizeof(struct perf_event_header)) {
            quality->pebs_malformed++;
            tail = head;
            break;
        }
        uint64_t off = tail % data_size;

        struct perf_event_header hdr;
        ring_copy(&hdr, data, off, sizeof(hdr), data_size);
        if (hdr.size < sizeof(hdr) || hdr.size > head - tail || hdr.size > data_size) {
            quality->pebs_malformed++;
            tail = head;
            break;
        }

        if (hdr.type == PERF_RECORD_SAMPLE) {
            /*
             * Sample layout (ordered by sample_type bit position):
             *   PERF_SAMPLE_TID:      { u32 pid, tid; }
             *   PERF_SAMPLE_ADDR:     { u64 addr; }
             *   PERF_SAMPLE_WEIGHT_STRUCT: { union perf_sample_weight; }  LDLAT only
             *   PERF_SAMPLE_DATA_SRC:      { u64 data_src; }              LDLAT only
             */
            struct {
                struct perf_event_header header;
                uint32_t pid;
                uint32_t tid;
                uint64_t addr;
                uint64_t weight;
                uint64_t data_src;
            } rec;
            bool ldlat = agg->pact_ctx->use_ldlat;
            size_t need = ldlat ? sizeof(rec) : offsetof(__typeof__(rec), weight);
            if (hdr.size >= need) {
                ring_copy(&rec, data, off, need, data_size);
                int w = sample_workload(agg, cpu_state, (pid_t)rec.pid, (pid_t)rec.tid);
                uint32_t latency = 0;
                if (w >= 0 && ldlat) {
                    /* Instruction latency where reported, else var1_dw. */
                    union perf_sample_weight pw = {.full = rec.weight};
                    latency = pw.var2_w ? pw.var2_w : pw.var1_dw;
                    if (!ldlat_keep(agg, w, latency, rec.data_src)) {
                        w = -1;
                    }
                }
                if (w >= 0) {
                    events[count++] = (staged_sample_t){
                        .page = rec.addr & PAGE_MASK, .workload = (uint32_t)w, .latency = latency};
                    agg->samples[w]++;
                    agg->total_samples++;
                }
            } else {
                quality->pebs_malformed++;
            }
        } else if (hdr.type == PERF_RECORD_LOST) {
            struct {
                struct perf_event_header header;
                uint64_t id, lost;
            } rec;
            if (hdr.size >= sizeof(rec)) {
                ring_copy(&rec, data, off, sizeof(rec), data_size);
                quality->pebs_lost_records++;
                quality->pebs_lost_samples += rec.lost;
            } else {
                quality->pebs_malformed++;
            }
        } else if (hdr.type == PERF_RECORD_LOST_SAMPLES) {
            struct {
                struct perf_event_header header;
                uint64_t lost;
            } rec;
            if (hdr.size >= sizeof(rec)) {
                ring_copy(&rec, data, off, sizeof(rec), data_size);
                quality->pebs_lost_records++;
                quality->pebs_lost_samples += rec.lost;
            } else {
                quality->pebs_malformed++;
            }
        } else if (hdr.type == PERF_RECORD_THROTTLE) {
            quality->pebs_throttles++;
            if (hdr.size < sizeof(hdr) + 3 * sizeof(uint64_t)) {
                quality->pebs_malformed++;
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
/* Per-sample stall attribution = the cycle's slow-tier scalar.
 * PEBS_ENCODE_PAC packs the PAC value into 18 bits, so clamp to 262143 here:
 * a larger value would be silently truncated by the encoder and corrupt the
 * PAC score. */
#define PAC_VALUE_MAX 262143u
static inline uint32_t compute_sample_stalls(double slow_stalls)
{
    if (slow_stalls < 0.0) {
        return 0;
    }
    if (slow_stalls > (double)PAC_VALUE_MAX) {
        return PAC_VALUE_MAX;
    }
    return (uint32_t)slow_stalls;
}

/* Global attributed stalls = k_constant * llc_misses / (mlp * events), with
 * the LLC misses and samples of every workload and the global MLP; every
 * sample of the window gets the same scalar. The /pebs_sampling_period
 * division is intentionally absent: ring drops and lost samples break
 * the ideal ev≈miss/period identity, so we let PAC scale with observed
 * sample population instead. */
static void compute_attributed_stalls(pact_context_t *ctx, pebs_aggregator_t *agg, double *stalls)
{
    if (ctx->mode == PACT_MODE_TIERCE) {
        /* Tierce: S_w = k * LLCmiss_w / (MLP_w * N_w * P) per workload, with
         * the workload MLP and the counts of this window. */
        for (int w = 0; w < ctx->nr_workloads; w++) {
            pact_workload_t *wl = &ctx->workloads[w];
            double coeff = wl->mlp * (double)agg->samples[w] * (double)ctx->pebs_sampling_period;
            stalls[w] =
                coeff > 0
                    ? ((double)ctx->k_constant_cxl * (double)wl->stats.llc_misses_slow) / coeff
                    : 0.0;
        }
        return;
    }
    uint64_t llc_misses = 0;
    for (int w = 0; w < ctx->nr_workloads; w++) {
        llc_misses += ctx->workloads[w].stats.llc_misses_slow;
    }
    double global = 0.0;
    double coeff = ctx->global_mlp * (double)agg->total_samples;
    if (coeff > 0) {
        global = ((double)ctx->k_constant_cxl * (double)llc_misses) / coeff;
    }
    for (int w = 0; w < ctx->nr_workloads; w++) {
        stalls[w] = global;
    }
}

/* LDLAT: after the window's samples were scored, move each workload's
 * k_slow toward the mean latency of its samples that passed the
 * data-source filter (EWMA weight 0.3 for the new mean, floor 1 cycle). A
 * workload without such samples keeps its k_slow. */
#define LDLAT_K_SLOW_BETA 0.3
static void update_ldlat_baselines(pebs_aggregator_t *agg, pact_context_t *ctx)
{
    for (int w = 0; w < ctx->nr_workloads; w++) {
        if (agg->ldlat_latency_count[w] == 0) {
            continue;
        }
        pact_workload_t *wl = &ctx->workloads[w];
        double mean = (double)agg->ldlat_latency_sum[w] / (double)agg->ldlat_latency_count[w];
        wl->k_slow = LDLAT_K_SLOW_BETA * mean + (1.0 - LDLAT_K_SLOW_BETA) * wl->k_slow;
        if (wl->k_slow < 1.0) {
            wl->k_slow = 1.0;
        }
    }
}

/*
 * Drain, attribute, and publish one window's PEBS samples.
 *
 * Phase A drains every CPU's perf buffer into cycle_buf so the window's
 * sample population is known; phase B then computes the slow-tier
 * stall scalar from THIS window's counters and counts (Algorithm 1: each
 * sample gets S * A_p / A_t, with all terms from the same window) and pushes each
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
            staged_sample_t scratch[512];
            int dropped;
            while ((dropped = read_cpu_pebs_events(agg, cpu_state, scratch, 512)) > 0) {
                agg->read_events_from_perf += dropped;
                agg->dropped_events_update_full += dropped;
                ctx->stats.pebs_update_drops += dropped;
                agg->dropped_events_workload += dropped;
            }
        }
    }
    agg->last_cpu_checked = (agg->last_cpu_checked + 1) % agg->num_cpus;

    /* Phase B: same-window attribution. The sample counts were populated
     * by the phase-A reads; the LLC-miss and MLP inputs were read from the
     * PMU for this same window just before this call. */
    double stalls[MAX_WORKLOADS] = {0};
    compute_attributed_stalls(ctx, agg, stalls);

    /* Tierce keeps one ring slot free for the entry that ends the epoch. */
    bool tierce = ctx->mode == PACT_MODE_TIERCE;
    size_t room =
        ctx->pac_update_ring->size - 1 - ring_buffer_pac_update_size(ctx->pac_update_ring);
    if (tierce) {
        room = room > 0 ? room - 1 : 0;
    }
    int pac_count = 0;
    for (int i = 0; i < n; i++) {
        const staged_sample_t *sample = &agg->cycle_buf[i];
        double sample_stall = stalls[sample->workload];
        if (ctx->use_ldlat) {
            /* S_p = S_w * latency / k_slow, with k_slow from before this
             * window's update (update_ldlat_baselines runs after scoring). */
            sample_stall =
                sample_stall * (double)sample->latency / ctx->workloads[sample->workload].k_slow;
        }
        uint32_t attributed = compute_sample_stalls(sample_stall);
        log_pebs_sample(ctx, "pebs_aggregate_events", (int)sample->workload, sample->page,
                        PMU_TIER_SLOW, attributed);

        pac_update_t update = {.encoded = PEBS_ENCODE_PAC(attributed, sample->page, PMU_TIER_SLOW),
                               .workload = sample->workload};
        if ((size_t)pac_count >= room ||
            !ring_buffer_pac_update_push(ctx->pac_update_ring, update)) {
            /* Ring full; the adaptive coroutine drains it on this same
             * thread, so nothing frees up mid-loop. Drop the rest. */
            agg->dropped_events_update_full += (uint64_t)(n - i);
            ctx->stats.pebs_update_drops += (uint64_t)(n - i);
            break;
        }
        agg->pushed[sample->workload]++;
        pac_count++;
    }
    if (tierce && pac_count > 0) {
        pac_update_t end = {.encoded = 0, .workload = PAC_UPDATE_EPOCH_END};
        ring_buffer_pac_update_push(ctx->pac_update_ring, end);
    }

    agg->pushed_events_to_update += pac_count;
    if (ctx->use_ldlat) {
        update_ldlat_baselines(agg, ctx);
    }

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
    memset(agg->samples, 0, sizeof(agg->samples));
    memset(agg->pushed, 0, sizeof(agg->pushed));
    memset(agg->ldlat_latency_sum, 0, sizeof(agg->ldlat_latency_sum));
    memset(agg->ldlat_latency_count, 0, sizeof(agg->ldlat_latency_count));
    agg->total_samples = 0;
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
    for (int w = 0; w < ctx->nr_workloads; w++) {
        pact_workload_t *wl = &ctx->workloads[w];
        uint64_t theory = wl->stats.llc_misses_slow / ctx->pebs_sampling_period;
        log_pebs_aggregator(ctx, "pebs_aggregator_coroutine", w, theory, agg->samples[w],
                            agg->lost_events, agg->lost_samples, 0, agg->dropped_events_workload);
    }
}

/* Refresh the task groups of every live workload while counters are
 * stopped. A workload exits only when its pidfd reports the exit: it is
 * marked exited and leaves sampling, and its groups close once. A refresh
 * that finds every thread gone while the pidfd has not fired (the threads
 * are exiting, PF_EXITING, but the process has not reached exit_notify())
 * keeps the workload: it has no group in the next window, and the next
 * window refreshes it again. Any
 * other failure stops sampling. Returns -1 when sampling must stop. */
static int refresh_counting_groups(pact_context_t *ctx)
{
    if (ctx->mode == PACT_MODE_TIERCE) {
        return 0; /* per-CPU groups do not depend on the task population */
    }
    for (int w = 0; w < ctx->nr_workloads; w++) {
        pact_workload_t *wl = &ctx->workloads[w];
        if (!wl->exited) {
            int refreshed = setup_workload_counting_events(wl);
            if (refreshed == 0) {
                continue;
            }
            if (refreshed == PACT_WORKLOAD_GONE) {
                if (!pact_workload_exited(ctx, wl)) {
                    if (wl->threads) {
                        cleanup_workload_counting_events(wl); /* clears wl->threads */
                    }
                    if (ctx->sampling_failed) {
                        return -1; /* the pidfd query failed */
                    }
                    log_debug("pebs_aggregator_coroutine",
                              "Workload %d (PID %d): no live thread, but its pidfd reports no "
                              "exit; it stays",
                              w, (int)wl->target_pid);
                    continue;
                }
            } else if (!pact_workload_exited(ctx, wl)) {
                if (!ctx->sampling_failed) {
                    log_error("pebs_aggregator_coroutine",
                              "Failed to refresh task counter coverage of workload %d", w);
                    ctx->sampling_failed = true;
                    ctx->running = false;
                }
                return -1;
            }
        }
        if (wl->threads) {
            cleanup_workload_counting_events(wl); /* clears wl->threads */
        }
    }
    return 0;
}

/* PEBS coroutine: one sampling window per tick, all CPUs through the
 * aggregator */
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
        /* Fresh sample counts for this window; phase A of
         * pebs_aggregate_events repopulates them from the drained samples
         * so attribution divides by this window's population. */
        reset_per_cycle_counters(agg);

        /* turn off pmu counters */
        stop_pmu_perf_events(ctx);
        if (ctx->sampling_failed) {
            break;
        }

        /* read counting events, e.g. LLC stalls, CHA stats */
        read_pmu_counting_events(ctx);

        if (poll_and_update_threads(ctx, debug_counter)) {
            log_info("pebs_aggregator_coroutine", "All workloads have exited, stopping PACT");
            ctx->running = false;
            mco_yield(co);
            break;
        }

        /* Keep the old task population and counters stopped until its samples
         * have been attributed. Newly created tasks join the next window. */
        int aggregated = pebs_aggregate_events(agg, ctx);
        if (ctx->mode == PACT_MODE_TIERCE) {
            /* The window's epoch-end marker is in the PAC ring: run the PAC
             * step next, so admission follows the window at once. */
            ctx->timing[CORO_TYPE_PAC].next_tsc = rdtsc();
        }
        if (refresh_counting_groups(ctx) < 0) {
            break;
        }
        if (pact_check_all_targets_exited(ctx)) {
            ctx->running = false;
            break;
        }
        start_pmu_perf_events(ctx);
        if (ctx->mode != PACT_MODE_TIERCE) {
            /* PACT: the next window starts one sampling interval after this
             * window's work. */
            ctx->timing[CORO_TYPE_PEBS].next_tsc =
                rdtsc() + ms_to_tsc(ctx, ctx->sampling_interval_ms);
        }
        /* Tierce keeps the deadline that the event loop set when this tick
         * started: a PAC epoch ends every sampling interval (20 ms by
         * default). */

        for (int w = 0; w < ctx->nr_workloads; w++) {
            pact_workload_t *wl = &ctx->workloads[w];
            /* Accumulate per-workload PEBS samples into cumulative counter */
            wl->total_pebs_samples += agg->samples[w];
            wl->stats.pebs_events_processed += agg->pushed[w];
            ctx->domains[wl->domain].samples += agg->pushed[w];
            /* Log workload PMU stats (after aggregation so event counts are available) */
            double mlp = (ctx->mode == PACT_MODE_TIERCE) ? wl->mlp : ctx->global_mlp;
            log_pmu(ctx, "pebs_aggregator_coroutine", w, mlp, wl->stats.llc_misses_slow,
                    agg->samples[w]);
        }

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
            log_debug("pebs_aggregator_coroutine", "PEBS aggregator: Pushed %d events to PAC ring",
                      aggregated);
            debug_counter = 0;
        }

        log_workload_pebs_stats(ctx, agg);

        mco_yield(co);
    }

    log_info("pebs_aggregator_coroutine", "PEBS Aggregator: Exiting");
}

/* Setup PEBS aggregator for the PACT context */
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

    for (int w = 0; w < ctx->nr_workloads; w++) {
        log_info("setup_pebs_aggregator", "PEBS filtering enabled for PID %d",
                 (int)ctx->workloads[w].target_pid);
    }
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
