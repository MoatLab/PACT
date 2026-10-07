/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* pact.c - Performance-criticality Aware Memory Tiering System */
/* _GNU_SOURCE already defined by compiler flags */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <numa.h>
#include <numaif.h>
#include "constants.h"
#include "error.h"
#include "pmu.h"
#include "pact.h"
#include <immintrin.h> /* _mm_pause */
#include "obj-pool.h"
#include "pebs-aggregator.h"
#include "logging.h"
#include "sighandler.h"
#include "config.h"
#include "validate.h"
#include "cli.h"
#include "tsc.h"
#include "balance.h"
#include "binning.h"
#include "stats.h"
#include "perf.h"
#include "stats-coro.h"
#include "cooling.h"
#include "utils.h"

/* layout invariants — failures would silently break NMI-shared state,
 * pool sizing math, or ring-buffer cache-line guarantees. */
_Static_assert(sizeof(pac_metadata_t) <= 256, "pac_metadata_t grew past 256 bytes — pool budget "
                                              "math and cache locality claims need updating");
_Static_assert(sizeof(migration_entry_t) <= 64,
               "migration_entry_t must fit in one cache line for ring buffer fairness");

#define MINICORO_IMPL
#include "minicoro.h"

/* Constants pact.c references directly (canonical defaults live in
 * pact_config.h as PACT_DEFAULT_*). */
#define RESERVOIR_SIZE 100 /* reservoir sample count for adaptive binning */
#define CORO_STACK_SIZE (64 * 1024)

void update_pac_entry(pact_context_t *pact, uint64_t page_addr, uint64_t stalls, uint8_t tier,
                      pid_t pid);

pact_context_t *g_pact_ctx = NULL;
#define g_pact g_pact_ctx

/* PAC update ring capacity: power-of-2 entries for SPSC queue design. */
#define PAC_UPDATE_RING_SIZE 131072

/* Alloc / free PAC metadata via the object pool. */
pac_metadata_t *alloc_pac_metadata(pact_context_t *pact)
{
    if (!pact) {
        return calloc(1, sizeof(pac_metadata_t));
    }

    /* Enforce pool capacity cap to prevent OOM at aggressive periods */
    if (pact->max_pac_entries > 0) {
        /* Heap fallback still has one metadata object per tracked page. */
        size_t in_use = pact->pac_metadata_pool ? get_total_capacity(pact->pac_metadata_pool) -
                                                      pool_available(pact->pac_metadata_pool)
                                                : kh_size(pact->workload->pac_table);
        if (in_use >= pact->max_pac_entries) {
            pact->workload->stats.pool_alloc_skipped++;
            /* Rate-limit: warn at first skip + at most once per second.
             * Summary line printed at exit in print_stats(). */
            uint64_t now = rdtsc();
            if (pact->workload->stats.pool_alloc_skipped == 1 ||
                (pact->tsc_freq_hz > 0 &&
                 now - pact->workload->stats.pool_warn_last_tsc >= pact->tsc_freq_hz)) {
                pact->workload->stats.pool_warn_last_tsc = now;
                log_warning(
                    "alloc_pac_metadata",
                    "PAC metadata pool full (%zu/%zu), skipping new page (total skipped: %lu)",
                    in_use, pact->max_pac_entries, pact->workload->stats.pool_alloc_skipped);
            }
            return NULL;
        }
    }

    if (!pact->pac_metadata_pool) {
        return calloc(1, sizeof(pac_metadata_t));
    }

    pac_metadata_t *meta = (pac_metadata_t *)pool_alloc_zero(pact->pac_metadata_pool);
    if (!meta) {
        /* Pool grow failed (system OOM) — skip rather than crash */
        pact->workload->stats.pool_alloc_skipped++;
        log_warning("alloc_pac_metadata",
                    "Pool alloc failed (system OOM), skipping page (total skipped: %lu)",
                    pact->workload->stats.pool_alloc_skipped);
        return NULL;
    }

    return meta;
}

/* Free PAC metadata to object pool */
/* Non-static: see alloc_pac_metadata. */
void free_pac_metadata(pact_context_t *pact, pac_metadata_t *meta)
{
    if (!pact || !pact->pac_metadata_pool || !meta) {
        free(meta); /* Fallback to free */
        return;
    }

    pool_free(pact->pac_metadata_pool, meta);
}

/* init/cleanup_logging live in pact_logging.c — these wrappers gate on
 * runtime ctx->enable_logging and the PACT_ENABLE_LOGGING compile flag. */
static void init_logging_wrapper(pact_context_t *pact PACT_UNUSED, const char *format PACT_UNUSED,
                                 const pact_config_t *config PACT_UNUSED)
{
#ifdef PACT_ENABLE_LOGGING
    if (!pact->enable_logging) {
        return;
    }

    const char *filename = (config->log_file[0] != '\0') ? config->log_file : NULL;
    init_logging(pact, filename, format);
#endif
}

static void close_logging(pact_context_t *pact PACT_UNUSED)
{
#ifdef PACT_ENABLE_LOGGING
    cleanup_logging(pact);
#endif
}

/* Reservoir sampling */
reservoir_t *reservoir_create(size_t capacity)
{
    reservoir_t *r = safe_calloc(1, sizeof(reservoir_t), "reservoir_create");
    r->samples = safe_calloc(capacity, sizeof(double), "reservoir_create");
    r->capacity = capacity;
    fast_prng_init(&r->prng);
    return r;
}

void reservoir_add_sample(reservoir_t *r, double value)
{
    if (r->count < r->capacity) {
        r->samples[r->count++] = value;
    } else {
        /* Classical reservoir sampling with fast xorshift PRNG. */
        uint64_t random_val = fast_prng_next(&r->prng);
        size_t idx = random_val % (r->total_seen + 1);
        if (idx < r->capacity) {
            r->samples[idx] = value;
        }
    }
    r->total_seen++;
}

/* Update tier and detect simple ping-pong patterns. */
static inline void update_tier_with_pingpong_detection(pact_context_t *pact, pac_metadata_t *meta,
                                                       uint8_t new_tier)
{
    uint8_t old_tier = meta->tier;

    /* Only check for ping-pong if tier actually changed */
    if (old_tier != new_tier) {
        /* Check for repromotion pattern: 0→1→0 */
        if (meta->prev_tier == 0 && old_tier == 1 && new_tier == 0) {
            pact->workload->stats.total_repromotions++;
        }
        /* Check for redemption pattern: 1→0→1 */
        else if (meta->prev_tier == 1 && old_tier == 0 && new_tier == 1) {
            pact->workload->stats.total_redemotions++;
        }
    }
    /* Shift tier history: current becomes prev */
    meta->prev_tier = old_tier;

    /* Update current tier */
    meta->tier = new_tier;
}

/* Promotion: page just arrived on fast tier. Drop any pending PQ entry
 * (coroutine path — thread mode checks meta->tier before migrating) and
 * if external mechanisms (kswapd, NUMA balancing) drove the move, account
 * the re-promotion category. */
static void on_repromote_to_fast(pact_context_t *pact, pac_metadata_t *meta)
{
    /* Page is back on fast tier — any pending migration entry in the ring
     * will be a no-op when the migration thread sees meta->tier == 0. We
     * don't try to dequeue from the FIFO ring (FIFO has no random-access
     * removal); the thread handles already-on-target pages. */
    if (!(meta->sampled_on_slow || meta->demoted_by_pact || meta->demoted_by_other)) {
        return;
    }
    if (meta->demoted_by_pact) {
        pact->workload->stats.repromotions_pact_other++; /* PACT demoted → other promoted */
        meta->demoted_by_pact = false;
    }
    if (meta->demoted_by_other) {
        pact->workload->stats.repromotions_other_other++; /* other demoted → other promoted */
        meta->demoted_by_other = false;
    }
    meta->promoted_by_other = true;
}

/* Demotion: page just landed on slow tier. If PACT had promoted it, fast-
 * path re-enqueue (temporal locality — recently-hot tends to stay hot).
 * Account redemotion category. */
static void on_redemote_to_slow(pact_context_t *pact, pac_metadata_t *meta)
{
    if (!(meta->sampled_on_fast || meta->promoted_by_pact || meta->promoted_by_other)) {
        return;
    }
    if (meta->promoted_by_pact) {
        pact->workload->stats.redemotions_pact_other++; /* PACT promoted → other demoted */
        meta->promoted_by_pact = false;
    }
    if (meta->promoted_by_other) {
        pact->workload->stats.redemotions_other_other++; /* other promoted → other demoted */
        meta->promoted_by_other = false;
    }
    meta->demoted_by_other = true;
}

static void handle_tier_change(pact_context_t *pact, pac_metadata_t *meta, uint8_t new_tier)
{
    if (new_tier == 0) {
        on_repromote_to_fast(pact, meta);
    } else if (new_tier == 1) {
        on_redemote_to_slow(pact, meta);
    }
}

/* Resolve the workload's pac_table / reservoir / binning in one call. */
static inline void pac_resolve_workload_data(pact_context_t *pact, pac_table_t **table,
                                             reservoir_t **res, binning_state_t **bin)
{
    *table = pact->workload->pac_table;
    *res = pact->workload->reservoir;
    *bin = pact->workload->binning;
}

/* Cold first-sight branch — keep out-of-line to avoid bloating the hot path. */
static pac_metadata_t *create_pac_entry(pact_context_t *pact, pac_table_t *table,
                                        uint64_t page_addr, uint8_t tier, pid_t pid)
{
    pac_metadata_t *meta = alloc_pac_metadata(pact);
    if (!meta) {
        return NULL;
    }
    meta->page_addr = page_addr;
    meta->pid = pid;
    meta->tier = tier;
    meta->prev_tier = -1;

    int ret;
    khint_t k = pac_table_put(table, page_addr, &ret);
    if (ret < 0) {
        log_error("update_pac_entry", "Failed to insert into hash table");
        free_pac_metadata(pact, meta);
        return NULL;
    }
    kh_val(table, k) = meta;
    return meta;
}

/* Apply this sample to an existing PAC entry: bump PAC value / access count,
 * handle tier transitions, and stamp the per-tier "sampled" flags. */
static inline void apply_sample_to_meta(pact_context_t *pact, pac_metadata_t *meta, uint64_t stalls,
                                        uint8_t tier)
{
    meta->pac_value += stalls;
    meta->access_count += 1;

    if (tier != meta->tier) {
        handle_tier_change(pact, meta, tier);
        update_tier_with_pingpong_detection(pact, meta, tier);
    }
    if (tier == 0) {
        meta->sampled_on_fast = 1;
        meta->sampled_on_slow = 0;
    } else {
        meta->sampled_on_slow = 1;
        meta->sampled_on_fast = 0;
    }
}

/* Per-sample reservoir update (Algorithm 3). */
static inline void update_sample_reservoir(reservoir_t *res, pac_metadata_t *meta)
{
    reservoir_add_sample(res, (double)meta->pac_value);
}

/* Promotion-queue gating decision.
 *
 * Threshold policy: only the top bin enters, with no hysteresis. */
static inline bool should_enter_promotion_pq(pac_metadata_t *meta, binning_state_t *bin,
                                             size_t bin_index)
{
    return bin && bin_index >= (size_t)(bin->bin_count - 1) && meta->tier == 1;
}

/* Coalesce a burst after publishing queue entries. The worker acknowledges
 * before checking the queue again, so a racing enqueue is seen or wakes it. */
static void wake_migration_worker(pact_context_t *ctx)
{
    if (atomic_exchange(&ctx->migration_wake_pending, true)) {
        return;
    }
    uint64_t one = 1;
    ssize_t result;
    do {
        result = write(ctx->migration_wake_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN) {
        atomic_store(&ctx->migration_error, errno);
    }
}

static void consume_migration_wakeup(pact_context_t *ctx)
{
    uint64_t count;
    ssize_t result;
    do {
        result = read(ctx->migration_wake_fd, &count, sizeof(count));
    } while (result < 0 && errno == EINTR);
    if (result == sizeof(count)) {
        atomic_exchange(&ctx->migration_wake_pending, false);
    } else if (result < 0 && errno != EAGAIN) {
        atomic_store(&ctx->migration_error, errno);
    }
}

void update_pac_entry(pact_context_t *pact, uint64_t page_addr, uint64_t stalls, uint8_t tier,
                      pid_t pid)
{
    pac_table_t *table;
    reservoir_t *res;
    binning_state_t *bin;
    pac_resolve_workload_data(pact, &table, &res, &bin);

    khint_t k = pac_table_get(table, page_addr);
    pac_metadata_t *meta;

    if (k != kh_end(table)) {
        meta = kh_val(table, k);
    } else {
        meta = create_pac_entry(pact, table, page_addr, tier, pid);
        if (!meta) {
            return;
        }
    }

    apply_sample_to_meta(pact, meta, stalls, tier);

    pact->workload->stats.pac_updates += 1;

    size_t bin_index = (size_t)-1;
    if (bin && bin->bin_width > 0) {
        double index = meta->pac_value / bin->bin_width;
        /* Small positive widths can exceed size_t, including infinity.
         * Such scores belong to the top bin; avoid an undefined cast. */
        bin_index = index < (double)SIZE_MAX ? (size_t)index : SIZE_MAX;
    }
    const char *tier_str = (meta->tier == 0) ? "fast" : (meta->tier == 1) ? "slow" : "unknown";
    log_pac_update(pact, "update_pac_entry", pact->workload ? 0 : -1, page_addr, meta->pac_value,
                   tier_str, bin_index);

    update_sample_reservoir(res, meta);

    if (should_enter_promotion_pq(meta, bin, bin_index) && !meta->migrating) {
        /* Push directly into migration_ring. meta->migrating gate prevents
         * double-enqueue; migration thread clears it after numa_move_pages. */
        meta->migrating = true;
        ring_buffer_migration_entry_t *ring = pact->workload->migration_ring;
        migration_entry_t entry = {.meta = meta, .target_node = 0};
        if (ring_buffer_migration_entry_push(ring, entry)) {
            atomic_inc_relaxed(&pact->workload->stats.promotion_attempts);
            wake_migration_worker(pact);
        } else {
            meta->migrating = false; /* ring full — let next sample retry */
        }
    }

    /* EMA of avg PAC for optimization heuristics. */
    pact->workload->stats.avg_pac = (pact->workload->stats.avg_pac * 15 + meta->pac_value) >> 4;
}

/* Migration thread: drains the workload ring and waits for notifications
 * when idle. Dispatches numa_move_pages synchronously. */

/* Process per-page numa_move_pages results — must be thread-safe vs main thread.
 * Every entry in the migration ring is a promotion (target_node=0); demotion
 * is handled by the kernel's demotion_enabled toggle, not numa_move_pages. */
static void process_migration_batch_results(pact_context_t *ctx, pac_metadata_t **metas,
                                            int *status, int count, long result, int errno_val)
{
    if (result < 0 && !(result == -1 && errno_val == ENOMEM)) {
        log_error("migration_thread", "numa_move_pages failed: %ld, errno=%d", result, errno_val);
    }

    uint64_t promotion_success = 0, demotion_success = 0;

    /* Process individual page results. Early-continue on failure keeps the
     * success path at the outer indent level (Linux kernel coding-style §7
     * centralized-exit guidance). */
    for (int i = 0; i < count; i++) {
        pac_metadata_t *meta = metas[i];
        if (!meta) {
            continue; /* synthetic neighbor — no metadata to update */
        }

        if (status[i] == INT_MIN) {
            /* Neither migration nor the residency query supplied a result. */
            meta->migrating = false;
            atomic_inc_relaxed(&ctx->workload->stats.promotion_failures);
            log_debug("migration_thread", "Page %p migration outcome is unknown",
                      (void *)meta->page_addr);
            continue;
        }
        if (status[i] < 0) {
            /* Promotion failed — mark as not migrating so it can be re-selected. */
            meta->migrating = false;
            atomic_inc_relaxed(&ctx->workload->stats.promotion_failures);
            log_trace("migration_thread", "Page %p migration failed with status %d, error is:%s",
                      (void *)meta->page_addr, status[i], strerror(-status[i]));
            continue;
        }

        uint8_t old_tier = meta->tier;
        uint8_t new_tier = status[i];

        if (old_tier != new_tier) {
            /* Update tier - atomic store for thread safety */
            meta->tier = new_tier;

            /* Update stats atomically */
            if (new_tier == 0) {
                /* Promotion */
                atomic_inc_relaxed(&ctx->workload->stats.promotion_successes);
                /* Ping-pong detection */
                if (meta->demoted_by_pact) {
                    atomic_inc_relaxed(&ctx->workload->stats.repromotions_pact_pact);
                }
                if (meta->demoted_by_other) {
                    atomic_inc_relaxed(&ctx->workload->stats.repromotions_other_pact);
                }
                meta->promoted_by_pact = true;
                meta->promoted_by_other = false;
                meta->demoted_by_pact = false;
                meta->demoted_by_other = false;
                promotion_success++;
            } else {
                /* Demotion */
                atomic_inc_relaxed(&ctx->workload->stats.pact_demotions);
                if (meta->promoted_by_pact) {
                    atomic_inc_relaxed(&ctx->workload->stats.redemotions_pact_pact);
                }
                if (meta->promoted_by_other) {
                    atomic_inc_relaxed(&ctx->workload->stats.redemotions_other_pact);
                }
                meta->demoted_by_pact = true;
                meta->demoted_by_other = false;
                meta->promoted_by_pact = false;
                meta->promoted_by_other = false;
                demotion_success++;
            }

            /* Update prev_tier for ping-pong detection */
            meta->prev_tier = old_tier;
        }
        meta->migrating = false;
    }

    log_debug("migration_thread", "Batch processed: %lu promotions, %lu demotions, %d total",
              promotion_success, demotion_success, count);
}

/*
 * Dedicated migration thread - busy-waits on per-workload ring buffers
 * Executes numa_move_pages() directly without coroutine overhead
 */
/* Linux can leave status slots untouched after partial migration or a global
 * error. Recover only those slots with a bounded residency query. A destination
 * observation is not proof that this invocation physically copied the page. */
static void recover_migration_status(pid_t pid, void **pages, const int *nodes, int *status,
                                     int count)
{
    if (count <= 0) {
        return;
    }
    enum { QUERY_BATCH = 256 };
    void *pending[QUERY_BATCH];
    int indices[QUERY_BATCH], observed[QUERY_BATCH];
    int cursor = 0;
    while (cursor < count) {
        int n = 0;
        while (cursor < count && n < QUERY_BATCH) {
            if (status[cursor] == INT_MIN) {
                pending[n] = pages[cursor];
                indices[n] = cursor;
                observed[n++] = INT_MIN;
            }
            cursor++;
        }
        if (n == 0) {
            continue;
        }
        if (numa_move_pages(pid, n, pending, NULL, observed, 0) < 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            int index = indices[i];
            if (observed[i] == INT_MIN) {
                continue;
            }
            /* A page still outside its requested tier remains retryable. */
            status[index] = observed[i] >= 0 && observed[i] != nodes[index] ? -EAGAIN : observed[i];
        }
    }
}

/* Dispatch one already-assembled batch via synchronous numa_move_pages. */
static void mig_dispatch_batch(pact_context_t *ctx, pid_t target_pid, void **pages, int *nodes,
                               int *status, pac_metadata_t **metas, int batch_count)
{
    for (int i = 0; i < batch_count; i++) {
        status[i] = INT_MIN;
    }
    long result = numa_move_pages(target_pid, batch_count, pages, nodes, status, MPOL_MF_MOVE);
    int errno_val = errno;
    recover_migration_status(target_pid, pages, nodes, status, batch_count);
    process_migration_batch_results(ctx, metas, status, batch_count, result, errno_val);
}

/* Add one entry to the in-flight batch arrays. Returns false if the page
 * is already at its target tier (kernel migrated it between enqueue and
 * dequeue) — caller must not advance batch_count. */
static bool mig_add_entry_to_batch(const migration_entry_t *entry, void **pages, int *nodes,
                                   pac_metadata_t **metas, int *status, int idx)
{
    int expected_from_tier = (entry->target_node == 0) ? 1 : 0;
    if (entry->meta->tier != expected_from_tier) {
        entry->meta->migrating = false;
        return false;
    }
    pages[idx] = (void *)entry->meta->page_addr;
    nodes[idx] = entry->target_node;
    metas[idx] = entry->meta;
    status[idx] = -1;
    return true;
}

/* Drain the workload's migration ring. */
static int mig_drain_workload_ring(pact_context_t *ctx, void **pages, int *nodes, int *status,
                                   pac_metadata_t **metas, int max_batch)
{
    pact_workload_t *wl = ctx->workload;
    ring_buffer_migration_entry_t *ring = wl->migration_ring;
    pid_t wl_pid = wl->target_pid;
    int wl_migrated = 0;

    while (ring_buffer_migration_entry_size(ring) > 0) {
        int batch_limit = max_batch;
        int batch_count = 0;
        migration_entry_t entry;
        while (batch_count < batch_limit && ring_buffer_migration_entry_pop(ring, &entry)) {
            if (mig_add_entry_to_batch(&entry, pages, nodes, metas, status, batch_count)) {
                batch_count++;
            }
        }
        if (batch_count == 0) {
            break;
        }

        mig_dispatch_batch(ctx, wl_pid, pages, nodes, status, metas, batch_count);
        wl_migrated += batch_count;
    }
    return wl_migrated;
}

typedef struct {
    pact_context_t *ctx;
    void **pages;
    int *nodes;
    int *status;
    pac_metadata_t **metas;
} migration_worker_t;

static void free_migration_worker(migration_worker_t *worker)
{
    if (!worker) {
        return;
    }
    free(worker->pages);
    free(worker->nodes);
    free(worker->status);
    free(worker->metas);
    free(worker);
}

/* Allocate before pthread_create so failure reaches the main startup path. */
static migration_worker_t *alloc_migration_worker(pact_context_t *ctx)
{
    migration_worker_t *worker = calloc(1, sizeof(*worker));
    if (!worker) {
        return NULL;
    }
    worker->ctx = ctx;
    size_t count = ctx->max_migrations_per_cycle;
    worker->pages = malloc(count * sizeof(*worker->pages));
    worker->nodes = malloc(count * sizeof(*worker->nodes));
    worker->status = malloc(count * sizeof(*worker->status));
    worker->metas = malloc(count * sizeof(*worker->metas));
    if (!worker->pages || !worker->nodes || !worker->status || !worker->metas) {
        free_migration_worker(worker);
        return NULL;
    }
    return worker;
}

static void *migration_thread_fn(void *arg)
{
    migration_worker_t *worker = arg;
    pact_context_t *ctx = worker->ctx;

    if (ctx->migration_cpu >= 0) {
        pact_pin_to_cpu(ctx->migration_cpu);
    }

    int max_batch = ctx->max_migrations_per_cycle;
    void **pages = worker->pages;
    int *nodes = worker->nodes;
    int *status = worker->status;
    pac_metadata_t **metas = worker->metas;
    log_info("migration_thread", "Started (batch_size=%d)", max_batch);

    /* Periodic balance check at ~1Hz. Uses TSC for cadence so it scales
     * with whatever rate the drain loop runs at. */
    const uint64_t balance_interval_tsc = ctx->tsc_freq_hz; /* 1 second */
    uint64_t next_balance_tsc = rdtsc() + balance_interval_tsc;

    while (ctx->migration_thread_running) {
        if (rdtsc() >= next_balance_tsc) {
            check_migration_balance(ctx);
            next_balance_tsc = rdtsc() + balance_interval_tsc;
        }

        ring_buffer_migration_entry_t *ring = ctx->workload->migration_ring;
        if (ring_buffer_migration_entry_size(ring) > 0) {
            (void)mig_drain_workload_ring(ctx, pages, nodes, status, metas, max_batch);
        } else {
            struct pollfd wait_fd = {.fd = ctx->migration_wake_fd, .events = POLLIN};
            /* Bound idle waits so periodic balancing still runs without work. */
            int result = poll(&wait_fd, 1, 100);
            if (result > 0 && (wait_fd.revents & POLLIN)) {
                consume_migration_wakeup(ctx);
            } else if (result > 0) {
                atomic_store(&ctx->migration_error, EIO);
            } else if (result < 0 && errno != EINTR) {
                atomic_store(&ctx->migration_error, errno);
            }
            if (atomic_load(&ctx->migration_error)) {
                break;
            }
        }
    }

    free_migration_worker(worker);
    log_info("migration_thread", "Migration thread exiting");
    return NULL;
}

static int alloc_workload_migration_ring(pact_context_t *ctx, size_t ring_size)
{
    ctx->workload->migration_ring = ring_buffer_migration_entry_create(ring_size);
    if (!ctx->workload->migration_ring) {
        log_error("init_migration_thread", "Failed to create workload migration ring");
        return -1;
    }
    return 0;
}

static int init_migration_thread(pact_context_t *ctx)
{
    /* Burst mode can drain many PQ entries at once — keep the ring large. */
    size_t ring_size = MIGRATION_RING_DEFAULT_SIZE;

    if (alloc_workload_migration_ring(ctx, ring_size) < 0) {
        return -1;
    }

    migration_worker_t *worker = alloc_migration_worker(ctx);
    if (!worker) {
        log_error("init_migration_thread", "Failed to allocate worker batch arrays");
        ring_buffer_migration_entry_destroy(ctx->workload->migration_ring);
        ctx->workload->migration_ring = NULL;
        return -1;
    }

    ctx->migration_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->migration_wake_fd < 0) {
        free_migration_worker(worker);
        ring_buffer_migration_entry_destroy(ctx->workload->migration_ring);
        ctx->workload->migration_ring = NULL;
        return -1;
    }
    atomic_init(&ctx->migration_wake_pending, false);
    atomic_init(&ctx->migration_error, 0);
    ctx->migration_thread_running = true;
    int error = pthread_create(&ctx->migration_thread, NULL, migration_thread_fn, worker);
    if (error != 0) {
        ctx->migration_thread_running = false;
        close(ctx->migration_wake_fd);
        ctx->migration_wake_fd = -1;
        /* pthread_create returns an error number without setting errno. */
        errno = error;
        log_error("init_migration_thread", "Failed to create migration thread: %s",
                  strerror(error));
        ring_buffer_migration_entry_destroy(ctx->workload->migration_ring);
        ctx->workload->migration_ring = NULL;
        free_migration_worker(worker);
        return -1;
    }
    log_info("init_migration_thread", "Migration thread initialized (ring size: %zu)", ring_size);
    return 0;
}

/* Stop migration thread, free rings, close trace files. */
static void cleanup_migration_thread(pact_context_t *ctx)
{
    if (!ctx->migration_thread_running) {
        return;
    }

    /* Signal thread to stop */
    ctx->migration_thread_running = false;
    wake_migration_worker(ctx);

    /* Wait for thread to finish */
    pthread_join(ctx->migration_thread, NULL);
    close(ctx->migration_wake_fd);
    ctx->migration_wake_fd = -1;

    /* Cleanup ring buffer */
    if (ctx->workload->migration_ring) {
        ring_buffer_migration_entry_destroy(ctx->workload->migration_ring);
        ctx->workload->migration_ring = NULL;
    }

    log_info("cleanup_migration_thread", "Migration thread cleaned up");
}


/* Earliest deadline across all timed event-loop work. Used to size the idle
 * sleep so we wake exactly when the next coroutine (or the 1Hz target-alive
 * check) is due, instead of polling every millisecond. */
static uint64_t next_deadline_tsc(const pact_context_t *pact)
{
    uint64_t earliest = pact->targets_alive_next_tsc;
    for (coro_type_t t = 0; t < CORO_TYPE_MAX; t++) {
        uint64_t d = pact->timing[t].next_tsc;
        if (d && d < earliest) {
            earliest = d;
        }
    }
    return earliest;
}

/* Cooperative yield gate for coroutines that drain unbounded work.
 *
 * Returns true when either:
 *   (a) the calling coroutine has been resumed for longer than its own
 *       configured interval — burning further time would just push *its*
 *       next deadline later for no gain; or
 *   (b) some other coroutine's (or the 1Hz target-alive check's) deadline
 *       has already slipped past now — yielding lets the event loop fire it.
 *
 * Callers should mco_yield() when this returns true, then resume the drain on
 * the next tick. The deadline-first sleep in run_pact_event_loop guarantees
 * we'll wake promptly to finish.
 *
 * Note: deliberately not used by pebs_aggregator_coroutine — the per-cycle
 * PMU stop/read/start sequence around pebs_aggregate_events makes mid-cycle
 * yields unsafe (we would lose samples that arrive in the gap). Empirically
 * one cycle is sub-20us, so it doesn't need this. */
static inline bool coro_should_yield(const pact_context_t *pact, coro_type_t self)
{
    uint64_t now = rdtsc();
    uint64_t window = (uint64_t)pact_coro_interval_ms(pact, self) * pact->tsc_freq_hz / 1000;
    if (now - pact->timing[self].prev_tsc >= window) {
        return true;
    }
    for (coro_type_t t = 0; t < CORO_TYPE_MAX; t++) {
        if (t == self) {
            continue;
        }
        uint64_t d = pact->timing[t].next_tsc;
        if (d && d <= now) {
            return true;
        }
    }
    if (pact->targets_alive_next_tsc && pact->targets_alive_next_tsc <= now) {
        return true;
    }
    return false;
}

/* Apply one encoded PEBS sample from the PAC ring to the workload's table. */
static inline void apply_encoded_pac_sample(pact_context_t *ctx, uint64_t encoded)
{
    update_pac_entry(ctx, PEBS_DECODE_PAGE(encoded), PEBS_DECODE_PAC(encoded),
                     PEBS_DECODE_TIER(encoded), ctx->workload->target_pid);
}

static void adaptive_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);
    while (ctx->running) {
        /* Drain the PAC update ring in batches of up to PAC_DRAIN_BATCH. The
         * ring holds up to PAC_UPDATE_RING_SIZE (128K) entries, so the naive
         * drain-to-empty could process ~128 batches per resume and starve
         * other coroutines. After each batch, check coro_should_yield() and
         * mco_yield() if any other deadline has slipped — we'll be rescheduled
         * one event-loop iteration later and pick up where we left off. */
        while (ring_buffer_uint64_size(ctx->pac_update_ring) > 0) {
            uint64_t pages[PAC_DRAIN_BATCH];
            int count = ring_buffer_uint64_pop_batch(ctx->pac_update_ring, pages, PAC_DRAIN_BATCH);
            log_ring_buffer_op(ctx, "adaptive_coroutine", "pop_batch", "pac_update_ring", count,
                               ring_buffer_uint64_size(ctx->pac_update_ring),
                               (size_t)PAC_UPDATE_RING_SIZE);
            for (int i = 0; i < count; i++) {
                apply_encoded_pac_sample(ctx, pages[i]);
            }
            if (coro_should_yield(ctx, CORO_TYPE_PAC)) {
                mco_yield(co);
                if (!ctx->running) {
                    return;
                }
            }
        }
        update_bin_width(ctx);
        mco_yield(co);
    }
}

/* Create and initialize coroutines */
static int create_coroutine(pact_context_t *ctx, int type, const char *name,
                            void (*func)(mco_coro *))
{
    mco_desc desc = mco_desc_init(NULL, CORO_STACK_SIZE);
    desc.user_data = ctx;
    desc.func = func;
    if (mco_create(&ctx->coroutines[type], &desc) != MCO_SUCCESS) {
        log_error("init_coroutines", "Failed to create %s coroutine", name);
        return -1;
    }
    return 0;
}

static int init_coroutines(pact_context_t *ctx)
{
    if (ctx->pebs_available && ctx->pebs_aggregator) {
        if (create_coroutine(ctx, CORO_TYPE_PEBS, "PEBS aggregator", pebs_aggregator_coroutine) <
            0) {
            return -1;
        }
        log_info("init_coroutines", "PEBS aggregator coroutine created");
    }
    /* Migration runs in the dedicated pthread; no coroutine for it. */
    if (create_coroutine(ctx, CORO_TYPE_PAC, "adaptive", adaptive_coroutine) < 0) {
        return -1;
    }
    if (create_coroutine(ctx, CORO_TYPE_COOLING, "cooling", cooling_coroutine) < 0) {
        return -1;
    }
    if (create_coroutine(ctx, CORO_TYPE_STATS, "stats", stats_coroutine) < 0) {
        return -1;
    }
    log_info("init_coroutines", "Initialized 4 coroutines for single-threaded event loop");
    return 0;
}

/* Initialize PACT trace/scanner subsystems used by the event loop. Each
 * sub-init is non-fatal (continues without that trace); migration thread and
 * coroutines are fatal because they are load-bearing. Returns false on a
 * fatal init failure. */
static bool event_loop_init_subsystems(pact_context_t *pact)
{
    if (pact->monitor_cpu >= 0) {
        pact_pin_to_cpu(pact->monitor_cpu);
    }

    if (init_coroutines(pact) < 0) {
        log_error("pact_event_loop", "Failed to initialize coroutines");
        return false;
    }
    if (init_migration_thread(pact) < 0) {
        log_error("pact_event_loop", "Failed to initialize migration thread");
        return false;
    }
    return true;
}

static void event_loop_log_config(pact_context_t *pact)
{
    log_info("run_pact_event_loop", "Configuration:");
    log_info("run_pact_event_loop", "  Migration mode: dedicated thread (ring buffer)");
    log_info("run_pact_event_loop", "  Sample interval: %u ms", pact->sampling_interval_ms);
    log_info("run_pact_event_loop", "  Cooling interval: %u ms", pact->cooling_interval_ms);
    log_info("run_pact_event_loop", "  Adaptive interval: %u ms", pact->adaptive_interval_ms);
    log_info("run_pact_event_loop", "  Stats interval: %u ms", pact->stats_interval_ms);
    log_info("run_pact_event_loop", "  Max migrations per cycle: %u",
             pact->max_migrations_per_cycle);
}

/* Shared bookkeeping for a timer-driven coroutine tick: lateness accounting
 * against prev_tsc, advance prev_tsc, bump count. Caller separately resumes
 * the coroutine and updates next_tsc (which may follow custom logic like
 * burst mode). Returns the TSC snapshot used for the tick. */
static uint64_t coro_tick_account(pact_context_t *pact, coro_type_t type)
{
    struct coro_timing *t = &pact->timing[type];
    uint64_t interval_ms = pact_coro_interval_ms(pact, type);
    uint64_t start_tsc = rdtsc();
    uint64_t window = ms_to_tsc(pact, interval_ms);
    uint64_t lag = start_tsc - t->prev_tsc;
    if (lag > window + window / 20) { /* >5% late */
        t->late_counts++;
        t->late_tsc += lag;
        log_debug("run_pact_event_loop",
                  "Resume %s coroutine at tsc %lu, desired %lums, diff %lums", pact_coro_name(type),
                  start_tsc, interval_ms, tsc_to_ms(pact, lag));
    }
    t->prev_tsc = start_tsc;
    t->counts++;
    return start_tsc;
}

/* Standard timer-driven coroutine tick: schedule next, run book-keeping,
 * resume, log elapsed, and report errors. Returns true if the tick fired. */
static bool tick_coroutine(pact_context_t *pact, uint64_t now, coro_type_t type)
{
    struct coro_timing *t = &pact->timing[type];
    if (now < t->next_tsc) {
        return false;
    }
    uint64_t start_tsc = coro_tick_account(pact, type);
    t->next_tsc = start_tsc + ms_to_tsc(pact, pact_coro_interval_ms(pact, type));
    mco_result res = mco_resume(pact->coroutines[type]);
    uint64_t elapsed = tsc_to_us(pact, rdtsc() - start_tsc);
    log_coro_event(pact, "run_pact_event_loop", pact_coro_name(type), "resume", elapsed);
    if (res != MCO_SUCCESS) {
        log_error("pact_event_loop", "%s coroutine error", pact_coro_name(type));
    }
    return true;
}

static void check_targets_alive(pact_context_t *pact, uint64_t now)
{
    if (now < pact->targets_alive_next_tsc) {
        return;
    }
    if (pact_check_all_targets_exited(pact)) {
        log_info("run_pact_event_loop", "All target processes have exited, shutting down");
        pact->running = false;
    }
    pact->targets_alive_next_tsc = now + sec_to_tsc(pact, 1);
}


/* Main event loop with coroutines */
static bool run_pact_event_loop(pact_context_t *pact)
{
    log_info("run_pact_event_loop", "Starting PACT event loop (single-threaded with coroutines)");
    if (!event_loop_init_subsystems(pact)) {
        return false;
    }
    event_loop_log_config(pact);

    uint64_t loop_count = 0, total_yields = 0;

    while (pact->running) {
        if (atomic_load(&pact->migration_error)) {
            break;
        }
        uint64_t now = rdtsc();
        bool did_work = false;
        loop_count++;

        /* Note: each tick advances its own next_tsc BEFORE resuming the
         * coroutine — the coroutine may yield from several positions, so we
         * must not lose the scheduling slot if it runs long.
         *
         * Migration has its own helper (burst mode + thread/coroutine branch),
         * so iterate the other timed coroutines explicitly. */
        if (tick_coroutine(pact, now, CORO_TYPE_PEBS)) {
            total_yields++;
            did_work = true;
        }
        if (tick_coroutine(pact, now, CORO_TYPE_PAC)) {
            total_yields++;
            did_work = true;
        }
        if (tick_coroutine(pact, now, CORO_TYPE_COOLING)) {
            total_yields++;
            did_work = true;
        }
        if (tick_coroutine(pact, now, CORO_TYPE_STATS)) {
            total_yields++;
            did_work = true;
        }

        check_targets_alive(pact, now);

        /* Deadline-first idle sleep: when no coroutine fired this iteration,
         * sleep until the earliest pending deadline (next coroutine tick or
         * the 1Hz target-alive check) instead of polling every millisecond.
         * If a deadline has already slipped past `now`, skip the sleep so the
         * next iteration runs it immediately. EINTR from signal delivery
         * returns early; the loop's `running` check then unwinds normally. */
        if (!did_work) {
            uint64_t deadline = next_deadline_tsc(pact);
            uint64_t after = rdtsc();
            if (deadline > after) {
                usleep(tsc_to_us(pact, deadline - after));
            }
        }

        if (loop_count % 100000 == 0) {
            log_debug("run_pact_event_loop", "Event loop: %lu iterations, %lu coroutine yields",
                      loop_count, total_yields);
        }
    }

    log_info("run_pact_event_loop",
             "Exiting event loop after %lu iterations and %lu coroutine yields", loop_count,
             total_yields);

    cleanup_migration_thread(pact);
    for (int i = 0; i < CORO_TYPE_MAX; i++) {
        if (pact->coroutines[i]) {
            mco_destroy(pact->coroutines[i]);
            pact->coroutines[i] = NULL;
        }
    }
    int error = atomic_load(&pact->migration_error);
    if (error) {
        log_error("run_pact_event_loop", "Migration worker notification failed: %s",
                  strerror(error));
    }
    return error == 0;
}

/* Initialize per-CPU state arrays from the workload's affinity. */
static void init_cpu_states(pact_context_t *pact)
{
    pact->nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    pact->cpu_states = calloc(pact->nr_all_cpus, sizeof(per_cpu_state_t));
    for (int i = 0; i < pact->nr_all_cpus; i++) {
        per_cpu_state_t *cs = &pact->cpu_states[i];
        init_per_cpu_state(cs);
        cs->cpu_id = pact->all_cpus[i];
        cs->pebs_sampling_period = pact->pebs_sampling_period;
    }
    log_info("pact_init", "Initialized %d CPUs for workload", pact->nr_all_cpus);
}

/* Apply CLI-configured initial bin width/count to the workload's binning. */
static void init_workload_binning(pact_context_t *pact)
{
    pact->workload->binning->bin_width = pact->bin_width;
    pact->workload->binning->bin_count = pact->bin_count;
}

/* Stamp now_tsc into every coroutine's next/prev tsc slot so the event loop
 * fires each at one interval from now. */
static void init_coro_tick_clocks(pact_context_t *pact, uint64_t now)
{
    pact->start_tsc = now;
    for (coro_type_t t = 0; t < CORO_TYPE_MAX; t++) {
        pact->timing[t].next_tsc = now + ms_to_tsc(pact, pact_coro_interval_ms(pact, t));
        pact->timing[t].prev_tsc = now;
    }
    pact->targets_alive_next_tsc = now + sec_to_tsc(pact, 1);
}

/* PAC update ring + coroutine handle array. Startup OOM is fatal: every
 * sample flows through this ring, so there is nothing to degrade to. */
static void init_pac_update_ring(pact_context_t *pact)
{
    pact->pac_update_ring = ring_buffer_uint64_create(PAC_UPDATE_RING_SIZE);
    if (!pact->pac_update_ring) {
        log_error("pact_init", "Failed to initialize PAC update ring buffer");
        exit(EXIT_FAILURE);
    }
    memset(pact->coroutines, 0, sizeof(pact->coroutines));
}

/* Initialize the pool; configuration supplies the default or explicit limit. */
static void init_pac_metadata_pool(pact_context_t *pact)
{
    pact->pac_metadata_pool = pool_create(sizeof(pac_metadata_t), 1000, 500, false);
    if (!pact->pac_metadata_pool) {
        log_error("pact_init", "Failed to initialize PAC metadata pool");
    }
    log_info("pact_init", "PAC metadata pool: max_entries=%zu", pact->max_pac_entries);
}

static void pact_init(pact_context_t *pact)
{
    /* Per-workload counting events are set up in setup_pact_perf_events()
     * (called below from setup_pebs_aggregator → perf.c). No per-TID
     * discovery or pidmap registration is needed: the counting fds use
     * per-PID inherit, and PEBS attribution uses a direct TGID compare. */
    init_cpu_states(pact);

    if (setup_pebs_aggregator(pact, pact->cpu_states, pact->nr_all_cpus, pact->all_cpus_mask) < 0) {
        log_error("pact_init", "Failed to setup PEBS aggregator");
        pact->pebs_available = false;
    } else {
        log_info("pact_init", "PEBS aggregator initialized successfully");
    }

    init_workload_binning(pact);
    init_pac_update_ring(pact);
    init_coro_tick_clocks(pact, rdtsc());

    init_pac_metadata_pool(pact);
    if (pact->demotion_policy == DEMOTION_KERNEL_LRU) {
        pact->demotion_baseline = read_migration_stats("pgdemote_kswapd pgdemote_direct");
        pact->workload->stats.last_demotions_successes = pact->demotion_baseline;
    }
}

/* Cleanup */
static void destroy_traces(pact_context_t *pact)
{
    close_logging(pact);
}

static void destroy_coroutines(pact_context_t *pact)
{
    for (int i = 0; i < CORO_TYPE_MAX; i++) {
        if (pact->coroutines[i]) {
            mco_destroy(pact->coroutines[i]);
            pact->coroutines[i] = NULL;
        }
    }
}

/* Close per-CHA event-group fds for the workload. The CHA struct itself
 * lives inline in pact_workload_t. */
static void destroy_cha_state(pact_context_t *pact)
{
    if (!pact->workload) {
        return;
    }
    pact_workload_t *wl = pact->workload;
    for (int cha = 0; cha < wl->nr_cha; cha++) {
        cha_pmu_info_t *cha_pmu = &wl->cha_pmus[cha];
        for (int i = 0; i < cha_pmu->group_fast.counters_used; i++) {
            safe_close(cha_pmu->group_fast.fds[i], "pact_destroy");
        }
        for (int i = 0; i < cha_pmu->group_slow.counters_used; i++) {
            safe_close(cha_pmu->group_slow.fds[i], "pact_destroy");
        }
    }
}

/* Per-CPU PEBS teardown: stop+cleanup uPEBS if active, then munmap perf
 * buffers and close all per-cpu event fds. Frees the cpu_states array. */
static void destroy_per_cpu_pebs(pact_context_t *pact)
{
    for (int cpu = 0; cpu < pact->nr_all_cpus; cpu++) {
        per_cpu_state_t *cs = &pact->cpu_states[cpu];
        if (cs->pebs_mmap && cs->pebs_mmap != MAP_FAILED) {
            munmap(cs->pebs_mmap, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
        }
        safe_close(cs->fd_pebs, "pact_destroy");
        safe_close(cs->leader.fd, "pact_destroy"); /* per-CPU dummy group leader */
    }
    free(pact->cpu_states);
}

/* Workload counting fds + workload misc arrays (cpus/MLP). */
static void destroy_per_workload_perf(pact_context_t *pact)
{
    if (!pact->workload) {
        return;
    }
    pact_workload_t *wl = pact->workload;
    cleanup_workload_counting_events(wl);
    free(wl->target_cpus);
    wl->target_cpus = NULL;
}

/* Workload hash table, reservoir, binning. */
static void destroy_per_workload_data(pact_context_t *pact)
{
    if (!pact->workload) {
        return;
    }
    pact_workload_t *wl = pact->workload;
    if (wl->pac_table) {
        khint_t wk;
        kh_foreach(wl->pac_table, wk)
        {
            free_pac_metadata(pact, kh_val(wl->pac_table, wk));
        }
        pac_table_destroy(wl->pac_table);
    }
    if (wl->reservoir) {
        free(wl->reservoir->samples);
        free(wl->reservoir);
    }
    free(wl->binning);
}

void pact_destroy(pact_context_t *pact)
{
    if (!pact) {
        return;
    }

    destroy_traces(pact);
    if (pact->pac_update_ring) {
        ring_buffer_uint64_destroy(pact->pac_update_ring);
    }
    destroy_coroutines(pact);
    if (pact->pebs_aggregator) {
        pebs_aggregator_destroy(pact->pebs_aggregator);
        pact->pebs_aggregator = NULL;
    }
    destroy_cha_state(pact);
    destroy_per_cpu_pebs(pact);
    destroy_per_workload_perf(pact);
    destroy_per_workload_data(pact);
    free(pact->workload);
    pact->workload = NULL;
    free(pact->all_cpus);

    if (pact->pac_metadata_pool) {
        pool_destroy(pact->pac_metadata_pool);
    }

    safe_close(pact->target_pidfd, "pact_destroy");
    free(pact);
}

/* Initialize the workload from configuration. */
static int init_workload(pact_workload_t *wl, const pact_config_t *config, int max_cpus,
                         bool *cpu_used, uint64_t *all_cpus_mask)
{
    wl->target_pid = config->target_pid;
    strncpy(wl->name, config->workload_name, sizeof(wl->name) - 1);
    wl->name[sizeof(wl->name) - 1] = '\0';

    /* CPU mask: workload is pinned externally via taskset. Detect the
     * actual affinity at runtime via sched_getaffinity(target_pid) so
     * per-CPU PEBS + CHA discovery cover exactly the CPUs the workload
     * can run on (not the whole machine, which may include CPUs without
     * working PEBS).
     *
     * Fallback if sched_getaffinity(target) fails: use our own affinity
     * (online CPUs only) rather than all _SC_NPROCESSORS_CONF CPUs,
     * which may include offline CPUs that cause perf_event_open ENODEV. */
    int cap = max_cpus < 64 ? max_cpus : 64;
    cpu_set_t aff;
    CPU_ZERO(&aff);
    int got_aff = (sched_getaffinity((pid_t)wl->target_pid, sizeof(aff), &aff) == 0);
    if (!got_aff) {
        log_warning("init_workload",
                    "sched_getaffinity(PID %d) failed (errno %d: %s), "
                    "falling back to online CPUs",
                    (int)wl->target_pid, errno, strerror(errno));
        got_aff = (sched_getaffinity(0, sizeof(aff), &aff) == 0);
    }
    int n_cpus = 0;
    for (int j = 0; j < cap; j++) {
        if (got_aff ? CPU_ISSET(j, &aff) : 1) {
            n_cpus++;
        }
    }
    if (n_cpus == 0) {
        /*
         * The per-CPU state and CHA masks are indexed by a 64-bit CPU mask, so
         * only CPUs 0..63 are supported. If we read a valid affinity but none
         * of its CPUs fall in that range, the workload is pinned to CPUs >= 64;
         * silently falling back to CPUs 0..63 would monitor the wrong cores, so
         * fail clearly instead.
         */
        if (got_aff) {
            log_error("init_workload",
                      "Workload PID %d is pinned outside CPUs 0-63; PACT only "
                      "supports the first 64 CPUs. Pin the workload to CPUs 0-63.",
                      (int)wl->target_pid);
            return -1;
        }
        n_cpus = cap; /* no affinity info: defensively take all of 0..cap */
        got_aff = 0;
    }
    if (n_cpus <= 0 || n_cpus > 64) {
        log_error("init_workload", "Invalid CPU count %d (expected 1-64)", n_cpus);
        return -1;
    }
    wl->nr_target_cpus = n_cpus;
    wl->target_cpus = calloc((size_t)n_cpus, sizeof(int));
    if (!wl->target_cpus) {
        fprintf(stderr, "Error: alloc target_cpus for workload\n");
        return -1;
    }
    wl->cpu_mask = 0;
    int idx = 0;
    for (int j = 0; j < cap; j++) {
        if (got_aff && !CPU_ISSET(j, &aff)) {
            continue;
        }
        wl->target_cpus[idx++] = j;
        wl->cpu_mask |= (1ULL << j);
        cpu_used[j] = true;
        *all_cpus_mask |= (1ULL << j);
    }
    printf("Workload (PID %d) affinity: %d CPUs (mask=0x%lx)\n", (int)wl->target_pid, n_cpus,
           wl->cpu_mask);

    wl->workload_mlp_fast = 1.0;
    wl->workload_mlp_slow = 1.0;

    memset(&wl->stats, 0, sizeof(pact_stats_t));

    wl->pac_table = pac_table_init();
    if (!wl->pac_table) {
        log_error("init_workload", "Failed to allocate PAC table");
        return -1;
    }
    wl->reservoir = reservoir_create(RESERVOIR_SIZE);
    wl->binning = safe_calloc(1, sizeof(binning_state_t), "wl->binning");

    wl->nr_cha = 0;

    printf("Initialized workload: PID %d, name '%s' (%d CPUs)\n", wl->target_pid, wl->name,
           wl->nr_target_cpus);
    return 0;
}

static void cleanup_workload(pact_context_t *pact)
{
    if (!pact->workload) {
        return;
    }
    destroy_per_workload_data(pact);
    free(pact->workload->target_cpus);
    free(pact->workload);
    pact->workload = NULL;
}

static int build_workload_cpu_set(pact_context_t *pact, int max_cpus, const bool *cpu_used)
{
    pact->nr_all_cpus = 0;
    for (int cpu = 0; cpu < max_cpus; cpu++) {
        if (cpu_used[cpu]) {
            pact->nr_all_cpus++;
        }
    }

    if (pact->nr_all_cpus == 0) {
        return 0;
    }

    pact->all_cpus = calloc(pact->nr_all_cpus, sizeof(int));
    if (!pact->all_cpus) {
        fprintf(stderr, "Error: Failed to allocate aggregated CPU list\n");
        return -1;
    }
    int idx = 0;
    for (int cpu = 0; cpu < max_cpus; cpu++) {
        if (cpu_used[cpu]) {
            pact->all_cpus[idx++] = cpu;
        }
    }
    printf("Workload CPU set: %d unique CPUs\n", pact->nr_all_cpus);
    return 0;
}

static int initialize_workloads(pact_context_t *pact, const pact_config_t *config)
{
    if (config->target_pid <= 0) {
        fprintf(stderr, "Error: --workload PID is required\n");
        return -1;
    }

    pact->workload = calloc(1, sizeof(pact_workload_t));
    if (!pact->workload) {
        fprintf(stderr, "Error: Failed to allocate workload\n");
        return -1;
    }

    int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
    bool *cpu_used = calloc(max_cpus, sizeof(bool));
    if (!cpu_used) {
        fprintf(stderr, "Error: Failed to allocate CPU tracking array\n");
        free(pact->workload);
        pact->workload = NULL;
        return -1;
    }

    pact->all_cpus_mask = 0;
    if (init_workload(pact->workload, config, max_cpus, cpu_used, &pact->all_cpus_mask) != 0) {
        cleanup_workload(pact);
        free(cpu_used);
        return -1;
    }

    if (build_workload_cpu_set(pact, max_cpus, cpu_used) != 0) {
        cleanup_workload(pact);
        free(cpu_used);
        return -1;
    }

    free(cpu_used);
    return 0;
}

static void copy_policy_and_intervals(pact_context_t *pact, const pact_config_t *config)
{
    pact->cooling_alpha = config->cooling_alpha;
    pact->cooling_trigger_samples = config->cooling_trigger_samples;
    pact->demotion_policy = config->demotion_policy;
    pact->sampling_interval_ms = config->sampling_interval_ms;
    pact->cooling_interval_ms = config->cooling_interval_ms;
    pact->adaptive_interval_ms = config->adaptive_interval_ms;
    pact->stats_interval_ms = config->stats_interval_ms;
    pact->max_migrations_per_cycle = config->max_migrations_per_cycle;
    pact->demotion_margin = config->demotion_margin;
    pact->enable_logging = config->enable_logging;
}

static void init_migration_and_optimizations(pact_context_t *pact, const pact_config_t *config)
{
    atomic_init(&pact->migration_thread_running, false);
    pact->max_pac_entries = config->pac_pool_max;
    pact->monitor_cpu = config->monitor_cpu;
    pact->migration_cpu = config->migration_cpu;
}

static int initialize_pact_context(pact_context_t *pact, const pact_config_t *config)
{
    pact->target_pidfd = -1;
    pact->pebs_sampling_period = config->pebs_period;
    if (initialize_workloads(pact, config) != 0) {
        return -1;
    }

    copy_policy_and_intervals(pact, config);
    init_migration_and_optimizations(pact, config);

    if (config->enable_logging) {
        init_logging_wrapper(pact, config->log_format, config);
        if (pact->workload) {
            log_workload_info(pact, "init", 0, pact->workload->target_pid, pact->workload->name,
                              "");
        }
    }

    pact->sample_counts = 0;
    pact->bin_width = config->bin_width;
    pact->bin_count = config->bin_count;

    pact->tsc_freq_hz = pact_detect_tsc_frequency();
    printf("Detected TSC frequency: %.2f GHz\n", pact->tsc_freq_hz / 1e9);
    if (pmu_platform_init() < 0) {
        /*
         * Unknown CPU: the SKX-specific PMU event encodings, CHA-to-core
         * mapping, and k-constants would be programmed into an incompatible
         * PMU, producing plausible-but-invalid results. Abort by default.
         * Set PACT_ALLOW_UNKNOWN_PLATFORM=1 only if you know your CPU is
         * SKX-counter-compatible and accept the SKX fallback.
         */
        if (getenv("PACT_ALLOW_UNKNOWN_PLATFORM")) {
            log_warning("pact_init", "Unknown CPU platform; using SKX defaults "
                                     "(PACT_ALLOW_UNKNOWN_PLATFORM set) — results may be invalid");
        } else {
            log_error("pact_init", "Unknown CPU platform. PACT's PMU events/constants are "
                                   "calibrated for Intel Skylake-X; running on another CPU "
                                   "would yield invalid results. Set "
                                   "PACT_ALLOW_UNKNOWN_PLATFORM=1 to override at your own risk.");
            return -1;
        }
    }
    pact->k_constant_dram = g_pmu_platform.k_constant_dram;
    pact->k_constant_cxl = g_pmu_platform.k_constant_cxl;
    return 0;
}

int main(int argc, char *argv[])
{
    pact_config_t config;
    pact_init_config(&config);

    int parse_result = pact_parse_command_line_args(argc, argv, &config);
    if (parse_result != 0) {
        return (parse_result > 0) ? 0 : 1; /* >0 = --help; <0 = error */
    }

    /* Banner is printed only for an actual run, not for --help / --version. */
    printf("PACT: A Criticality-First Design for Tiered Memory\n");
    printf("==================================================\n\n");

    if (pact_validate_configuration(&config) != 0) {
        return 1;
    }
    if (!validate_hardware_access()) {
        log_error("main", "Hardware validation failed");
        return 1;
    }

    g_pact_ctx = safe_calloc(1, sizeof(pact_context_t), "calloc g_pact_ctx");
    if (initialize_pact_context(g_pact_ctx, &config) != 0) {
        free(g_pact_ctx);
        return 1;
    }
    pact_init(g_pact_ctx);

    printf("PACT initialized for workload PID %d across %d CPUs\n", g_pact->workload->target_pid,
           g_pact->nr_all_cpus);

    g_pact->target_pidfd = syscall(SYS_pidfd_open, g_pact->workload->target_pid, 0);
    if (g_pact->target_pidfd < 0) {
        log_error("main", "Cannot open workload pidfd: %s", strerror(errno));
        pact_destroy(g_pact);
        return 1;
    }
    pact_signal_install_handlers(&g_pact->running);
    if (setup_pact_perf_events(g_pact) < 0) {
        pact_destroy(g_pact);
        return 1;
    }
    g_pact->running = !g_pact->sampling_failed;

    printf("=== PACT Runtime Mode: Single-threaded Coroutines ===\n");
    printf("Architecture: Lock-free, event-driven, cooperative multitasking\n\n");

    bool event_loop_ok = run_pact_event_loop(g_pact);

    printf("\n=== Coroutine Shutdown Sequence ===\n");
    g_pact->running = false;
    print_stats(g_pact);
    printf("✓ Coroutines stopped gracefully\n");

    printf("Cleaning up PACT resources...\n");
    bool sampling_failed = g_pact->sampling_failed;
    pact_destroy(g_pact);

    int sig = pact_signal_received();
    if (sig) {
        printf("PACT shutdown complete (signal: %d).\n", sig);
    } else {
        printf("PACT shutdown complete.\n");
    }

    if (!event_loop_ok || sampling_failed) {
        return 1;
    }
    pact_signal_write_clean_marker(sig);
    return 0;
}
