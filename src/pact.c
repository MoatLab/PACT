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
#include "cgroup.h"
#include "admission.h"

/* layout invariants: failures would silently break NMI-shared state,
 * pool sizing math, or ring-buffer cache-line guarantees. */
_Static_assert(sizeof(pac_metadata_t) <= 256, "pac_metadata_t grew past 256 bytes: pool budget "
                                              "math and cache locality claims need updating");
_Static_assert(sizeof(migration_entry_t) <= 64,
               "migration_entry_t must fit in one cache line for ring buffer fairness");
_Static_assert(sizeof(pac_update_t) == 16,
               "pac_update_t is the 64-bit PAC word plus a 32-bit workload number");

#define MINICORO_IMPL
#include "minicoro.h"

/* Constants pact.c references directly (canonical defaults live in
 * config.h as PACT_DEFAULT_*). */
#define RESERVOIR_SIZE 100 /* reservoir sample count for adaptive binning */
#define CORO_STACK_SIZE (64 * 1024)

void update_pac_entry(pact_context_t *pact, int workload, uint64_t page_addr, uint64_t stalls,
                      uint8_t tier);

pact_context_t *g_pact_ctx = NULL;
#define g_pact g_pact_ctx

/* Alloc / free PAC metadata via the object pool. The cap
 * (--pac-pool-max) applies to each PAC table: the domain counts the
 * metadata objects it holds, from the pool or from the heap fallback. */
pac_metadata_t *alloc_pac_metadata(pact_context_t *pact, pac_domain_t *domain)
{
    if (!pact) {
        return calloc(1, sizeof(pac_metadata_t));
    }

    /* Enforce pool capacity cap to prevent OOM at aggressive periods */
    if (pact->max_pac_entries > 0) {
        size_t in_use = domain->nr_metadata;
        if (in_use >= pact->max_pac_entries) {
            pact->stats.pool_alloc_skipped++;
            /* Rate-limit: warn at first skip + at most once per second.
             * Summary line printed at exit in print_stats(). */
            uint64_t now = rdtsc();
            if (pact->stats.pool_alloc_skipped == 1 ||
                (pact->tsc_freq_hz > 0 &&
                 now - pact->stats.pool_warn_last_tsc >= pact->tsc_freq_hz)) {
                pact->stats.pool_warn_last_tsc = now;
                log_warning(
                    "alloc_pac_metadata",
                    "PAC metadata pool full (%zu/%zu), skipping new page (total skipped: %lu)",
                    in_use, pact->max_pac_entries, pact->stats.pool_alloc_skipped);
            }
            return NULL;
        }
    }

    pac_metadata_t *meta;
    if (!pact->pac_metadata_pool) {
        meta = calloc(1, sizeof(pac_metadata_t));
    } else {
        meta = (pac_metadata_t *)pool_alloc_zero(pact->pac_metadata_pool);
    }
    if (!meta) {
        /* Pool grow failed (system OOM): skip rather than crash */
        pact->stats.pool_alloc_skipped++;
        log_warning("alloc_pac_metadata",
                    "Pool alloc failed (system OOM), skipping page (total skipped: %lu)",
                    pact->stats.pool_alloc_skipped);
        return NULL;
    }
    domain->nr_metadata++;
    return meta;
}

/* Free PAC metadata to object pool */
/* Non-static: see alloc_pac_metadata. */
void free_pac_metadata(pact_context_t *pact, pac_domain_t *domain, pac_metadata_t *meta)
{
    if (meta && domain && domain->nr_metadata > 0) {
        domain->nr_metadata--;
    }
    if (!pact || !pact->pac_metadata_pool || !meta) {
        free(meta); /* Fallback to free */
        return;
    }

    pool_free(pact->pac_metadata_pool, meta);
}

/* init/cleanup_logging live in logging.c. These wrappers gate on
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

/* Statistics of the workload that owns a page. */
static inline pact_stats_t *owner_stats(pact_context_t *pact, const pac_metadata_t *meta)
{
    return &pact->workloads[meta->workload].stats;
}

/* Update tier and detect simple ping-pong patterns. */
static inline void update_tier_with_pingpong_detection(pact_context_t *pact, pac_metadata_t *meta,
                                                       uint8_t new_tier)
{
    uint8_t old_tier = meta->tier;

    /* Only check for ping-pong if tier actually changed */
    if (old_tier != new_tier) {
        /* Check for repromotion pattern: 0->1->0 */
        if (meta->prev_tier == 0 && old_tier == 1 && new_tier == 0) {
            owner_stats(pact, meta)->total_repromotions++;
        }
        /* Check for redemotion pattern: 1->0->1 */
        else if (meta->prev_tier == 1 && old_tier == 0 && new_tier == 1) {
            owner_stats(pact, meta)->total_redemotions++;
        }
    }
    /* Shift tier history: current becomes prev */
    meta->prev_tier = old_tier;

    /* Update current tier */
    meta->tier = new_tier;
}

/* Cold first-sight branch: keep out-of-line to avoid bloating the hot path. */
static pac_metadata_t *create_pac_entry(pact_context_t *pact, pac_domain_t *domain,
                                        pact_workload_t *wl, uint64_t page_addr, uint8_t tier)
{
    pac_metadata_t *meta = alloc_pac_metadata(pact, domain);
    if (!meta) {
        return NULL;
    }
    meta->page_addr = page_addr;
    meta->pid = wl->target_pid;
    meta->workload = (uint8_t)wl->index;
    meta->tier = tier;
    meta->prev_tier = -1;

    int ret;
    khint_t k = pac_table_put(domain->table, pac_key(meta->workload, page_addr), &ret);
    if (ret < 0) {
        log_error("update_pac_entry", "Failed to insert into hash table");
        free_pac_metadata(pact, domain, meta);
        return NULL;
    }
    kh_val(domain->table, k) = meta;
    return meta;
}

/* Apply this sample to an existing PAC entry: bump PAC value / access count
 * and record a tier change. */
static inline void apply_sample_to_meta(pact_context_t *pact, pac_metadata_t *meta, uint64_t stalls,
                                        uint8_t tier)
{
    meta->pac_value += stalls;
    meta->access_count += 1;

    if (tier != meta->tier) {
        update_tier_with_pingpong_detection(pact, meta, tier);
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

/* Tierce: apply the sample and list the page for this epoch's admission.
 * A new page is offered to the domain's page reservoir. Admission waits for
 * the end of the epoch (end_pac_epoch). */
static void update_tierce_entry(pact_context_t *pact, pact_workload_t *wl, pac_domain_t *domain,
                                uint64_t page_addr, uint64_t stalls, uint8_t tier)
{
    khint_t k = pac_table_get(domain->table, pac_key((unsigned int)wl->index, page_addr));
    pac_metadata_t *meta;
    if (k != kh_end(domain->table)) {
        meta = kh_val(domain->table, k);
    } else {
        meta = create_pac_entry(pact, domain, wl, page_addr, tier);
        if (!meta) {
            return;
        }
        page_reservoir_add(domain->pages, meta);
    }
    apply_sample_to_meta(pact, meta, stalls, tier);
    wl->stats.pac_updates += 1;
    domain->epoch_updates++;
    log_pac_update(pact, "update_pac_entry", wl->index, page_addr, meta->pac_value,
                   (meta->tier == 0) ? "fast" : "slow", -1); /* no bins */
    if (meta->tier == 1) {
        epoch_list_page(domain, meta, pact->pac_epoch);
    }
    pact->stats.avg_pac = (pact->stats.avg_pac * 15 + meta->pac_value) >> 4;
}

void update_pac_entry(pact_context_t *pact, int workload, uint64_t page_addr, uint64_t stalls,
                      uint8_t tier)
{
    pact_workload_t *wl = &pact->workloads[workload];
    pac_domain_t *domain = &pact->domains[wl->domain];
    if (pact->mode == PACT_MODE_TIERCE) {
        update_tierce_entry(pact, wl, domain, page_addr, stalls, tier);
        return;
    }
    pac_table_t *table = domain->table;
    reservoir_t *res = domain->reservoir;
    binning_state_t *bin = domain->binning;

    khint_t k = pac_table_get(table, pac_key((unsigned int)workload, page_addr));
    pac_metadata_t *meta;

    if (k != kh_end(table)) {
        meta = kh_val(table, k);
    } else {
        meta = create_pac_entry(pact, domain, wl, page_addr, tier);
        if (!meta) {
            return;
        }
    }

    apply_sample_to_meta(pact, meta, stalls, tier);

    wl->stats.pac_updates += 1;

    size_t bin_index = (size_t)-1;
    if (bin && bin->bin_width > 0) {
        double index = meta->pac_value / bin->bin_width;
        /* Small positive widths can exceed size_t, including infinity.
         * Such scores belong to the top bin; avoid an undefined cast. */
        bin_index = index < (double)SIZE_MAX ? (size_t)index : SIZE_MAX;
    }
    const char *tier_str = (meta->tier == 0) ? "fast" : (meta->tier == 1) ? "slow" : "unknown";
    log_pac_update(pact, "update_pac_entry", workload, page_addr, meta->pac_value, tier_str,
                   bin_index);

    update_sample_reservoir(res, meta);

    bool promote = should_enter_promotion_pq(meta, bin, bin_index) && !meta->migrating;
#ifdef PACT_DIAG
    promote = promote && !(pact->no_promote_mask & (1u << meta->workload)); /* --no-promote-wl */
#endif
    if (promote) {
        /* Push directly into migration_ring. meta->migrating gate prevents
         * double-enqueue; migration thread clears it after numa_move_pages. */
        meta->migrating = true;
        ring_buffer_migration_entry_t *ring = domain->migration_ring;
        migration_entry_t entry = {.meta = meta, .target_node = 0};
        if (ring_buffer_migration_entry_push(ring, entry)) {
            atomic_inc_relaxed(&wl->stats.promotion_attempts);
            wake_migration_worker(pact);
        } else {
            meta->migrating = false; /* ring full: let next sample retry */
            domain->ring_full_drops++;
        }
    }

    /* EMA of avg PAC for optimization heuristics. */
    pact->stats.avg_pac = (pact->stats.avg_pac * 15 + meta->pac_value) >> 4;
}

/* Migration thread: drains the workload ring and waits for notifications
 * when idle. Dispatches numa_move_pages synchronously. */

/* Process per-page numa_move_pages results: must be thread-safe vs main thread.
 * Every entry in the migration ring is a promotion (target_node=0). The
 * kernel demotes: each workload cgroup's kdemotiond makes room under its
 * fast-tier cap, not numa_move_pages. */
static void process_migration_batch_results(pact_context_t *ctx, pac_metadata_t **metas,
                                            int *status, int count, long result, int errno_val)
{
    if (result < 0 && !(result == -1 && errno_val == ENOMEM)) {
        log_error("migration_thread", "numa_move_pages failed: %ld, errno=%d", result, errno_val);
    }

    uint64_t promotion_success = 0;

    /* Process individual page results. Early-continue on failure keeps the
     * success path at the outer indent level (Linux kernel coding-style §7
     * centralized-exit guidance). */
    for (int i = 0; i < count; i++) {
        pac_metadata_t *meta = metas[i];
        if (!meta) {
            continue; /* synthetic neighbor: no metadata to update */
        }

        pact_stats_t *stats = owner_stats(ctx, meta);
        if (status[i] == INT_MIN) {
            /* Neither migration nor the residency query supplied a result. */
            meta->migrating = false;
            atomic_inc_relaxed(&stats->promotion_failures);
            log_debug("migration_thread", "Page %p migration outcome is unknown",
                      (void *)meta->page_addr);
            continue;
        }
        if (status[i] < 0) {
            /* Promotion failed: mark as not migrating so it can be re-selected. */
            meta->migrating = false;
            atomic_inc_relaxed(&stats->promotion_failures);
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
                atomic_inc_relaxed(&stats->promotion_successes);
                promotion_success++;
            }

            /* Update prev_tier for ping-pong detection */
            meta->prev_tier = old_tier;
        }
        meta->migrating = false;
    }

    log_debug("migration_thread", "Batch processed: %lu promotions, %d total", promotion_success,
              count);
}

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

/* Before a promotion call, ask the kernel for the node of each page of the
 * batch (move_pages(2) without target nodes: no page moves and no LRU
 * drain). A sample can list a page that is already on its target node (a
 * sample taken before the page's promotion, or a slow-tier sample with the
 * address of a fast-tier page). Such a page gets its node as its tier, is
 * not sent and is not a promotion. A page without a node (a negative
 * status) is not sent either: the call would fail on it the same way. The
 * other pages move to the front of the arrays. Returns the pages to send,
 * or -1 when the query fails as a whole; the caller then sends nothing. */
static int mig_drop_resident_pages(pact_context_t *ctx, pid_t target_pid, void **pages, int *nodes,
                                   int *status, pac_metadata_t **metas, int batch_count)
{
    for (int i = 0; i < batch_count; i++) {
        status[i] = INT_MIN;
    }
    if (numa_move_pages(target_pid, batch_count, pages, NULL, status, 0) < 0) {
        if (atomic_fetch_add(&ctx->residency_query_failures, 1) == 0) {
            log_warning("migration_thread",
                        "The node query before a promotion call failed for PID %d (%s); "
                        "the worker sends no batch whose query fails",
                        (int)target_pid, strerror(errno));
        }
        return -1;
    }
    int kept = 0;
    for (int i = 0; i < batch_count; i++) {
        pac_metadata_t *meta = metas[i];
        int node = status[i];
        if (node >= 0 && node != nodes[i]) {
            pages[kept] = pages[i];
            nodes[kept] = nodes[i];
            metas[kept] = meta;
            kept++;
            continue;
        }
        if (!meta) {
            continue;
        }
        pact_stats_t *stats = owner_stats(ctx, meta);
        if (node == nodes[i]) {
            /* Already on the target node: the runtime's tier was stale. */
            meta->prev_tier = meta->tier;
            meta->tier = (uint8_t)node;
            atomic_inc_relaxed(&stats->promotion_skips_resident);
        } else {
            atomic_inc_relaxed(&stats->promotion_failures);
            log_trace("migration_thread", "Page %p has no node (status %d)",
                      (void *)meta->page_addr, node);
        }
        meta->migrating = false;
    }
    return kept;
}

/* Dispatch one already-assembled batch via synchronous numa_move_pages.
 * The pages of an exited workload are not sent: its PID can belong to
 * another process by now. The node query before the call drops the pages
 * that are already on their target node. A page can still change node
 * between the query and the call. */
static void mig_dispatch_batch(pact_context_t *ctx, pid_t target_pid, void **pages, int *nodes,
                               int *status, pac_metadata_t **metas, int batch_count)
{
    if (batch_count > 0 && metas[0] && atomic_load(&ctx->workloads[metas[0]->workload].exited)) {
        for (int i = 0; i < batch_count; i++) {
            if (metas[i]) {
                metas[i]->migrating = false;
            }
        }
        return;
    }
    int count = mig_drop_resident_pages(ctx, target_pid, pages, nodes, status, metas, batch_count);
    if (count < 0) {
        for (int i = 0; i < batch_count; i++) {
            if (metas[i]) {
                metas[i]->migrating = false;
                atomic_inc_relaxed(&owner_stats(ctx, metas[i])->promotion_failures);
            }
        }
        return;
    }
    if (count == 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        status[i] = INT_MIN;
    }
    long result = numa_move_pages(target_pid, count, pages, nodes, status, MPOL_MF_MOVE);
    int errno_val = errno;
    recover_migration_status(target_pid, pages, nodes, status, count);
    process_migration_batch_results(ctx, metas, status, count, result, errno_val);
}

/* Add one entry to the in-flight batch arrays. Returns false if the page
 * is already at its target tier (kernel migrated it between enqueue and
 * dequeue): caller must not advance batch_count. */
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

/* Drain the global migration ring in numa_move_pages() calls of at most
 * max_batch pages. Each call moves the pages of one owner process: a page
 * of another process ends the current batch and starts the next one. */
static int mig_drain_ring(pact_context_t *ctx, ring_buffer_migration_entry_t *ring, void **pages,
                          int *nodes, int *status, pac_metadata_t **metas, int max_batch)
{
    int migrated = 0;
    int batch_count = 0;
    pid_t batch_pid = 0;
    migration_entry_t entry;

    while (ring_buffer_migration_entry_pop(ring, &entry)) {
        pid_t pid = entry.meta->pid;
        if (batch_count > 0 && (pid != batch_pid || batch_count == max_batch)) {
            mig_dispatch_batch(ctx, batch_pid, pages, nodes, status, metas, batch_count);
            migrated += batch_count;
            batch_count = 0;
        }
        if (mig_add_entry_to_batch(&entry, pages, nodes, metas, status, batch_count)) {
            batch_pid = pid;
            batch_count++;
        }
    }
    if (batch_count > 0) {
        mig_dispatch_batch(ctx, batch_pid, pages, nodes, status, metas, batch_count);
        migrated += batch_count;
    }
    return migrated;
}

/* Tierce: one round-robin pass over the domain rings. Each domain with
 * queued pages moves at most max_batch of them in one numa_move_pages()
 * call, then the next domain takes its turn. Returns the pages dispatched;
 * *found reports whether any ring held entries. */
static int mig_round_robin(pact_context_t *ctx, void **pages, int *nodes, int *status,
                           pac_metadata_t **metas, int max_batch, bool *found)
{
    int migrated = 0;
    *found = false;
    for (int d = 0; d < ctx->nr_domains; d++) {
        ring_buffer_migration_entry_t *ring = ctx->domains[d].migration_ring;
        migration_entry_t entry;
        int batch_count = 0;
        pid_t batch_pid = 0;
        for (int popped = 0; popped < max_batch && ring_buffer_migration_entry_pop(ring, &entry);
             popped++) {
            *found = true;
            if (mig_add_entry_to_batch(&entry, pages, nodes, metas, status, batch_count)) {
                batch_pid = entry.meta->pid;
                batch_count++;
            }
        }
        if (batch_count > 0) {
            mig_dispatch_batch(ctx, batch_pid, pages, nodes, status, metas, batch_count);
            migrated += batch_count;
        }
    }
    return migrated;
}

/* Most pages in one numa_move_pages() call. */
static int migration_batch_size(const pact_context_t *ctx)
{
    return (int)(ctx->mode == PACT_MODE_TIERCE ? ctx->domain_migration_batch
                                               : ctx->max_migrations_per_cycle);
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
    size_t count = (size_t)migration_batch_size(ctx);
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

/* Dedicated migration thread: drains the migration rings with synchronous
 * numa_move_pages() calls. With no queued page it sleeps in poll() on the
 * wake eventfd, at most 100 ms, so the 1 Hz demotion statistics still run. */
static void *migration_thread_fn(void *arg)
{
    migration_worker_t *worker = arg;
    pact_context_t *ctx = worker->ctx;

    /* A failed --migration-cpu pin stops the run: the event loop reads
     * migration_error. */
    if (ctx->migration_cpu >= 0 && pact_pin_to_cpu(ctx->migration_cpu) < 0) {
        atomic_store(&ctx->migration_error, errno);
        free_migration_worker(worker);
        return NULL;
    }

    int max_batch = migration_batch_size(ctx);
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

        bool found;
        if (ctx->mode == PACT_MODE_TIERCE) {
            (void)mig_round_robin(ctx, pages, nodes, status, metas, max_batch, &found);
        } else {
            ring_buffer_migration_entry_t *ring = ctx->domains[0].migration_ring;
            found = ring_buffer_migration_entry_size(ring) > 0;
            if (found) {
                (void)mig_drain_ring(ctx, ring, pages, nodes, status, metas, max_batch);
            }
        }
        if (!found) {
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

static void free_migration_rings(pact_context_t *ctx)
{
    for (int d = 0; d < ctx->nr_domains; d++) {
        ring_buffer_migration_entry_destroy(ctx->domains[d].migration_ring);
        ctx->domains[d].migration_ring = NULL;
    }
}

static int alloc_migration_rings(pact_context_t *ctx, size_t ring_size)
{
    for (int d = 0; d < ctx->nr_domains; d++) {
        ctx->domains[d].migration_ring = ring_buffer_migration_entry_create(ring_size);
        if (!ctx->domains[d].migration_ring) {
            log_error("init_migration_thread", "Failed to create migration ring");
            free_migration_rings(ctx);
            return -1;
        }
    }
    return 0;
}

static int init_migration_thread(pact_context_t *ctx)
{
    /* The migration worker can receive many queue entries at once: keep the
     * ring large. */
    size_t ring_size = MIGRATION_RING_DEFAULT_SIZE;

    if (alloc_migration_rings(ctx, ring_size) < 0) {
        return -1;
    }

    migration_worker_t *worker = alloc_migration_worker(ctx);
    if (!worker) {
        log_error("init_migration_thread", "Failed to allocate worker batch arrays");
        free_migration_rings(ctx);
        return -1;
    }

    ctx->migration_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->migration_wake_fd < 0) {
        free_migration_worker(worker);
        free_migration_rings(ctx);
        return -1;
    }
    atomic_init(&ctx->migration_wake_pending, false);
    atomic_init(&ctx->migration_error, 0);
    ctx->migration_thread_running = true;
    /* The thread inherits a mask with the shutdown signals blocked, so their
     * handler runs on the event-loop thread, which reads ctx->running. */
    sigset_t shutdown_signals, old_mask;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    sigaddset(&shutdown_signals, SIGHUP);
    int masked = pthread_sigmask(SIG_BLOCK, &shutdown_signals, &old_mask);
    int error = pthread_create(&ctx->migration_thread, NULL, migration_thread_fn, worker);
    if (masked == 0) {
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    }
    if (error != 0) {
        ctx->migration_thread_running = false;
        close(ctx->migration_wake_fd);
        ctx->migration_wake_fd = -1;
        /* pthread_create returns an error number without setting errno. */
        errno = error;
        log_error("init_migration_thread", "Failed to create migration thread: %s",
                  strerror(error));
        free_migration_rings(ctx);
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

    /* Cleanup ring buffers */
    free_migration_rings(ctx);

    log_info("cleanup_migration_thread", "Migration thread cleaned up");
}


/* Earliest deadline across all timed event-loop work. Used to size the idle
 * sleep so we wake exactly when the next coroutine (or the 1Hz target-alive
 * check) is due, instead of polling every millisecond. */
static uint64_t next_deadline_tsc(const pact_context_t *pact)
{
    uint64_t earliest = pact->targets_alive_next_tsc;
    if (pact->migration_trace.next_tsc && pact->migration_trace.next_tsc < earliest) {
        earliest = pact->migration_trace.next_tsc;
    }
#ifdef PACT_DIAG
    if (pact->pac_dump.next_tsc && pact->pac_dump.next_tsc < earliest) {
        earliest = pact->pac_dump.next_tsc;
    }
#endif
    if (pact->ldlat_dyn.next_tsc && pact->ldlat_dyn.next_tsc < earliest) {
        earliest = pact->ldlat_dyn.next_tsc;
    }
    if (pact->cat.next_tsc && pact->cat.next_tsc < earliest) {
        earliest = pact->cat.next_tsc;
    }
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
 *       configured interval: burning further time would just push *its*
 *       next deadline later for no gain; or
 *   (b) some other coroutine's (or the 1Hz target-alive check's) deadline
 *       has already slipped past now. Yielding lets the event loop fire it.
 *
 * Callers should mco_yield() when this returns true, then resume the drain on
 * the next tick. The deadline-first sleep in run_pact_event_loop guarantees
 * we'll wake promptly to finish.
 *
 * Note: deliberately not used by pebs_aggregator_coroutine. The per-cycle
 * PMU stop/read/start sequence around pebs_aggregate_events makes mid-cycle
 * yields unsafe (we would lose samples that arrive in the gap). */
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
    if (pact->ldlat_dyn.next_tsc && pact->ldlat_dyn.next_tsc <= now) {
        return true;
    }
    if (pact->cat.next_tsc && pact->cat.next_tsc <= now) {
        return true;
    }
    return false;
}

/* Apply one PAC update from the PAC ring to its workload's table. */
static inline void apply_pac_update(pact_context_t *ctx, const pac_update_t *update)
{
    if (update->workload == PAC_UPDATE_EPOCH_END) {
        if (end_pac_epoch(ctx) > 0) {
            wake_migration_worker(ctx);
        }
        return;
    }
    if (update->workload >= (uint32_t)ctx->nr_workloads) {
        log_warning("adaptive_coroutine", "Dropping update for unknown workload %u",
                    update->workload);
        return;
    }
    uint64_t encoded = update->encoded;
    update_pac_entry(ctx, (int)update->workload, PEBS_DECODE_PAGE(encoded),
                     PEBS_DECODE_PAC(encoded), PEBS_DECODE_TIER(encoded));
}

static void adaptive_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);
    while (ctx->running) {
        /* Drain the PAC update ring in batches of up to PAC_DRAIN_BATCH. The
         * ring holds up to PAC_UPDATE_RING_SIZE (128K) entries, so the naive
         * drain-to-empty could process ~128 batches per resume and starve
         * other coroutines. After each batch, check coro_should_yield() and
         * mco_yield() if any other deadline has slipped: we'll be rescheduled
         * one event-loop iteration later and pick up where we left off. */
        while (ring_buffer_pac_update_size(ctx->pac_update_ring) > 0) {
            pac_update_t updates[PAC_DRAIN_BATCH];
            int count =
                ring_buffer_pac_update_pop_batch(ctx->pac_update_ring, updates, PAC_DRAIN_BATCH);
            log_ring_buffer_op(ctx, "adaptive_coroutine", "pop_batch", "pac_update_ring", count,
                               ring_buffer_pac_update_size(ctx->pac_update_ring),
                               (size_t)PAC_UPDATE_RING_SIZE);
            for (int i = 0; i < count; i++) {
                apply_pac_update(ctx, &updates[i]);
            }
            if (coro_should_yield(ctx, CORO_TYPE_PAC)) {
                if (ctx->mode == PACT_MODE_TIERCE) {
                    /* The rest of this window's updates and its epoch-end
                     * marker are still queued: resume on the next event-loop
                     * pass, not one PAC interval later. */
                    ctx->timing[CORO_TYPE_PAC].next_tsc = rdtsc();
                }
                mco_yield(co);
                if (!ctx->running) {
                    return;
                }
            }
        }
        if (ctx->mode == PACT_MODE_PACT) {
            update_bin_width(ctx);
        }
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
    log_info("init_coroutines", "Initialized the event-loop coroutines");
    return 0;
}

/* Initialize PACT trace subsystems used by the event loop. A trace that
 * does not open is not fatal: the loop runs without it. A failed
 * --monitor-cpu pin, the migration thread and the coroutines are fatal
 * because they are load-bearing. Returns false on a fatal init failure. */
static bool event_loop_init_subsystems(pact_context_t *pact)
{
    if (pact->monitor_cpu >= 0 && pact_pin_to_cpu(pact->monitor_cpu) < 0) {
        return false;
    }
    if (pact->trace_migration_path[0] != '\0') {
        (void)migration_trace_open(pact, pact->trace_migration_path);
    }
#ifdef PACT_DIAG
    if (pact->dump_pac_table_path[0] != '\0') {
        (void)pac_dump_open(pact, pact->dump_pac_table_path, pact->dump_pac_interval_s);
    }
#endif

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
    log_info("run_pact_event_loop", "  Mode: %s (%d PAC tables)", pact_mode_name(pact->mode),
             pact->nr_domains);
    log_info("run_pact_event_loop", "  Migration mode: dedicated thread (ring buffer)");
    log_info("run_pact_event_loop", "  Sample interval: %u ms", pact->sampling_interval_ms);
    log_info("run_pact_event_loop", "  Cooling interval: %u ms", pact->cooling_interval_ms);
    log_info("run_pact_event_loop", "  Adaptive interval: %u ms", pact->adaptive_interval_ms);
    log_info("run_pact_event_loop", "  Stats interval: %u ms", pact->stats_interval_ms);
    if (pact->mode == PACT_MODE_TIERCE) {
        log_info("run_pact_event_loop", "  Domain migration batch: %u",
                 pact->domain_migration_batch);
    } else {
        log_info("run_pact_event_loop", "  Max migrations per cycle: %u",
                 pact->max_migrations_per_cycle);
    }
}

/* Shared bookkeeping for a timer-driven coroutine tick: lateness accounting,
 * advance prev_tsc, bump count. Caller separately resumes the coroutine and
 * updates next_tsc (which may follow custom logic). Returns
 * the TSC snapshot used for the tick.
 *
 * A tick is late when it starts more than 5% of its interval after its
 * deadline (next_tsc). Most deadlines are the previous start plus one
 * interval, so this equals a start-to-start time above 1.05 intervals. Two
 * deadlines differ. The PACT-mode sampling deadline is the end of the
 * previous window's work plus one interval, so that work does not count as
 * lateness. The Tierce PAC (adaptive) deadline is the end of each sampling
 * window (pebs_aggregator_coroutine), so its lateness counts from there.
 * late_tsc adds the start-to-start time of each late tick. */
static uint64_t coro_tick_account(pact_context_t *pact, coro_type_t type)
{
    struct coro_timing *t = &pact->timing[type];
    uint64_t interval_ms = pact_coro_interval_ms(pact, type);
    uint64_t start_tsc = rdtsc();
    uint64_t window = ms_to_tsc(pact, interval_ms);
    uint64_t lag = start_tsc - t->prev_tsc;
    if (start_tsc > t->next_tsc + window / 20) { /* >5% late */
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
    log_info("run_pact_event_loop", "Starting PACT event loop (coroutines, migration thread)");
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
         * coroutine. The coroutine may yield from several positions, so we
         * must not lose the scheduling slot if it runs long.
         *
         * Migration runs in its own thread, not in this loop. */
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
        if (pact->ldlat_dyn.next_tsc && now >= pact->ldlat_dyn.next_tsc) {
            ldlat_dyn_tick(pact);
            pact->ldlat_dyn.next_tsc = now + sec_to_tsc(pact, 1);
        }
        if (pact->cat.next_tsc && now >= pact->cat.next_tsc) {
            cat_control_step(pact);
            pact->cat.next_tsc = now + ms_to_tsc(pact, pact->cat.cfg.interval_ms);
        }
        if (pact->migration_trace.next_tsc && now >= pact->migration_trace.next_tsc) {
            migration_trace_row(pact);
            pact->migration_trace.next_tsc = now + sec_to_tsc(pact, 1);
        }
#ifdef PACT_DIAG
        if (pact->pac_dump.next_tsc && now >= pact->pac_dump.next_tsc) {
            pac_dump_snapshot(pact);
            pact->pac_dump.next_tsc = rdtsc() + sec_to_tsc(pact, pact->pac_dump.interval_s);
        }
#endif

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
        log_error("run_pact_event_loop", "Migration failed: %s", strerror(error));
    }
    return error == 0;
}

/* Initialize per-CPU state arrays from the workload's affinity. Returns 0,
 * or -1 when the array cannot be allocated. */
static int init_cpu_states(pact_context_t *pact)
{
    pact->nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    pact->cpu_states = calloc(pact->nr_all_cpus, sizeof(per_cpu_state_t));
    if (!pact->cpu_states) {
        log_error("pact_init", "Cannot allocate the state of %d CPUs", pact->nr_all_cpus);
        return -1;
    }
    for (int i = 0; i < pact->nr_all_cpus; i++) {
        per_cpu_state_t *cs = &pact->cpu_states[i];
        init_per_cpu_state(cs);
        cs->cpu_id = pact->all_cpus[i];
        cs->pebs_sampling_period = pact->pebs_sampling_period;
        cs->mlp = g_pmu_platform.mlp_min; /* no measurement yet */
        for (int w = 0; w < pact->nr_workloads; w++) {
            if (cs->cpu_id < 64 && (pact->workloads[w].cpu_mask & (1ULL << cs->cpu_id))) {
                cs->owner = w;
                break;
            }
        }
    }
    log_info("pact_init", "Initialized %d CPUs for workload", pact->nr_all_cpus);
    return 0;
}

/* Apply CLI-configured initial bin width/count to each PAC table's binning. */
static void init_domain_binning(pact_context_t *pact)
{
    for (int d = 0; d < pact->nr_domains; d++) {
        if (!pact->domains[d].binning) {
            continue; /* Tierce tables have no bins */
        }
        pact->domains[d].binning->bin_width = pact->bin_width;
        pact->domains[d].binning->bin_count = pact->bin_count;
    }
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
    pact->pac_update_ring = ring_buffer_pac_update_create(PAC_UPDATE_RING_SIZE);
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

/* Snapshot each workload cgroup's pgdemote count, so the demotion
 * statistics count this run's demotions from time zero. */
static void init_demotion_baselines(pact_context_t *pact)
{
    for (int w = 0; w < pact->nr_workloads; w++) {
        pact_workload_t *wl = &pact->workloads[w];
        uint64_t demoted = 0;
        if (read_cgroup_demotions(wl->cgroup_path, &demoted) < 0) {
            log_warning("pact_init", "Cannot read pgdemote of %s; counting from 0",
                        wl->cgroup_path);
        }
        wl->demotion_baseline = demoted;
    }
}

/* Returns 0, or -1 when the per-CPU state or the PEBS aggregator cannot be
 * created: a run without sampling would score no page, so it does not start. */
static int pact_init(pact_context_t *pact)
{
    /* The counting events are set up later, in setup_pact_perf_events()
     * (perf.c): per-task groups of every workload thread in PACT mode, and
     * one group per workload CPU in Tierce mode. PEBS attribution uses a
     * direct TGID compare. */
    if (init_cpu_states(pact) < 0) {
        return -1;
    }

    if (setup_pebs_aggregator(pact, pact->cpu_states, pact->nr_all_cpus, pact->all_cpus_mask) < 0) {
        log_error("pact_init", "Failed to setup PEBS aggregator");
        pact->pebs_available = false;
        return -1;
    }
    log_info("pact_init", "PEBS aggregator initialized successfully");

    init_domain_binning(pact);
    init_pac_update_ring(pact);
    init_coro_tick_clocks(pact, rdtsc());

    init_pac_metadata_pool(pact);
    init_demotion_baselines(pact);
    return 0;
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

/* Close per-CHA event-group fds. The CHA structs live inline in the
 * context. */
static void destroy_cha_state(pact_context_t *pact)
{
    for (int cha = 0; cha < pact->nr_cha; cha++) {
        cha_pmu_info_t *cha_pmu = &pact->cha_pmus[cha];
        for (int i = 0; i < cha_pmu->group.counters_used; i++) {
            safe_close(cha_pmu->group.fds[i], "pact_destroy");
        }
    }
}

/* Per-CPU teardown: munmap the PEBS buffers and close every per-CPU event
 * fd (PEBS, its leader, and the counting group). Frees the cpu_states
 * array. */
static void destroy_per_cpu_pebs(pact_context_t *pact)
{
    if (!pact->cpu_states) {
        return; /* the allocation failed in pact_init() */
    }
    for (int cpu = 0; cpu < pact->nr_all_cpus; cpu++) {
        per_cpu_state_t *cs = &pact->cpu_states[cpu];
        if (cs->pebs_mmap && cs->pebs_mmap != MAP_FAILED) {
            munmap(cs->pebs_mmap, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
        }
        safe_close(cs->fd_pebs, "pact_destroy");
        safe_close(cs->leader.fd, "pact_destroy"); /* per-CPU dummy group leader */
        close_cpu_counting_group(cs);
    }
    free(pact->cpu_states);
    pact->cpu_states = NULL;
}

/* Workload counting fds, CPU arrays and pidfds. */
static void destroy_per_workload_perf(pact_context_t *pact)
{
    for (int w = 0; w < pact->nr_workloads; w++) {
        pact_workload_t *wl = &pact->workloads[w];
        cleanup_workload_counting_events(wl);
        free(wl->target_cpus);
        wl->target_cpus = NULL;
        safe_close(wl->pidfd, "pact_destroy");
        wl->pidfd = -1;
    }
}

/* PAC tables (with their metadata), reservoirs and binning. */
static void destroy_domains(pact_context_t *pact)
{
    for (int d = 0; d < pact->nr_domains; d++) {
        pac_domain_t *domain = &pact->domains[d];
        if (domain->table) {
            khint_t wk;
            kh_foreach(domain->table, wk)
            {
                free_pac_metadata(pact, domain, kh_val(domain->table, wk));
            }
            pac_table_destroy(domain->table);
        }
        if (domain->reservoir) {
            free(domain->reservoir->samples);
            free(domain->reservoir);
        }
        free(domain->binning);
        page_reservoir_destroy(domain->pages);
        free(domain->epoch_pages);
    }
    free(pact->domains);
    pact->domains = NULL;
    pact->nr_domains = 0;
}

void pact_destroy(pact_context_t *pact)
{
    pact_signal_clear_running_flag();
    if (!pact) {
        return;
    }

    destroy_traces(pact);
    ldlat_dyn_cleanup(pact);
    cat_cleanup(pact);
    migration_trace_close(pact);
#ifdef PACT_DIAG
    pac_dump_close(pact);
#endif
    if (pact->pac_update_ring) {
        ring_buffer_pac_update_destroy(pact->pac_update_ring);
    }
    destroy_coroutines(pact);
    if (pact->pebs_aggregator) {
        pebs_aggregator_destroy(pact->pebs_aggregator);
        pact->pebs_aggregator = NULL;
    }
    destroy_cha_state(pact);
    destroy_per_cpu_pebs(pact);
    destroy_per_workload_perf(pact);
    destroy_domains(pact);
    free(pact->workloads);
    pact->workloads = NULL;
    free(pact->all_cpus);

    if (pact->pac_metadata_pool) {
        pool_destroy(pact->pac_metadata_pool);
    }

    free(pact);
}

/* Read the CPU set of a workload whose CPUS word was not given: the
 * process affinity, capped to CPUs 0-63. Returns 0, or -1 with a message. */
static int read_workload_affinity(pact_workload_t *wl, int max_cpus, uint64_t *mask)
{
    /* CPU mask: workload is pinned externally via taskset. Detect the
     * actual affinity at runtime via sched_getaffinity(target_pid) so
     * per-CPU PEBS cover exactly the CPUs the workload can run on (not
     * the whole machine, which may include CPUs without working PEBS).
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
    uint64_t cpus = 0;
    for (int j = 0; j < cap; j++) {
        if (got_aff ? CPU_ISSET(j, &aff) : 1) {
            cpus |= 1ULL << j;
        }
    }
    if (cpus == 0) {
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
        cpus = (cap >= 64) ? UINT64_MAX : (1ULL << cap) - 1; /* no affinity info: take 0..cap */
    }
    *mask = cpus;
    return 0;
}

/* Initialize one workload from its --workload configuration. With a CPUS
 * word the given set replaces the affinity read. */
static int init_workload(pact_workload_t *wl, const pact_workload_config_t *wcfg, int index,
                         int domain, int max_cpus, bool *cpu_used, uint64_t *all_cpus_mask)
{
    wl->index = index;
    wl->domain = domain;
    wl->pidfd = -1;
    atomic_init(&wl->exited, false);
    wl->target_pid = wcfg->pid;
    snprintf(wl->name, sizeof(wl->name), "%s", wcfg->name);
    if (pact_cgroup_path(wcfg->cgroup, wl->cgroup_path, sizeof(wl->cgroup_path)) < 0) {
        log_error("init_workload", "Workload %d: cgroup path '%s' is too long", index,
                  wcfg->cgroup);
        return -1;
    }

    uint64_t cpus = 0;
    bool given = wcfg->cpus[0] != '\0' && strcmp(wcfg->cpus, PACT_CPUS_AUTO) != 0;
    if (given) {
        if (pact_parse_cpu_list(wcfg->cpus, &cpus) < 0) {
            log_error("init_workload", "Workload %d: invalid CPU list '%s'", index, wcfg->cpus);
            return -1;
        }
    } else if (read_workload_affinity(wl, max_cpus, &cpus) < 0) {
        return -1;
    }
    int n_cpus = __builtin_popcountll(cpus);
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
    wl->cpu_mask = cpus;
    int idx = 0;
    for (int j = 0; j < 64; j++) {
        if (!(cpus & (1ULL << j))) {
            continue;
        }
        wl->target_cpus[idx++] = j;
        if (j < max_cpus) {
            cpu_used[j] = true;
        }
        *all_cpus_mask |= (1ULL << j);
    }
    printf("Workload %d (PID %d) %s: %d CPUs (mask=0x%lx)\n", index, (int)wl->target_pid,
           given ? "CPU list" : "affinity", n_cpus, wl->cpu_mask);

    memset(&wl->stats, 0, sizeof(pact_stats_t));
    wl->mlp = 0.0; /* set to mlp_min once the platform is known */

    printf("Initialized workload %d: PID %d, name '%s' (%d CPUs), cgroup %s\n", index,
           wl->target_pid, wl->name, wl->nr_target_cpus, wl->cgroup_path);
    return 0;
}

/* Allocate one PAC table with its admission state: PACT's reservoir and
 * binning, or Tierce's page reservoir. */
static int init_domain(pac_domain_t *domain, pact_mode_t mode)
{
    domain->table = pac_table_init();
    if (!domain->table) {
        log_error("init_workload", "Failed to allocate PAC table");
        return -1;
    }
    if (mode == PACT_MODE_TIERCE) {
        domain->pages = page_reservoir_create(TIERCE_RESERVOIR_PAGES);
        return 0;
    }
    domain->reservoir = reservoir_create(RESERVOIR_SIZE);
    domain->binning = safe_calloc(1, sizeof(binning_state_t), "domain->binning");
    return 0;
}

static void cleanup_workloads(pact_context_t *pact)
{
    destroy_domains(pact);
    if (pact->workloads) {
        for (int w = 0; w < pact->nr_workloads; w++) {
            free(pact->workloads[w].target_cpus);
        }
    }
    free(pact->workloads);
    pact->workloads = NULL;
    pact->nr_workloads = 0;
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
    if (config->nr_workloads <= 0 || config->nr_workloads > MAX_WORKLOADS) {
        fprintf(stderr, "Error: --workload PID CPUS NAME CGROUP is required\n");
        return -1;
    }

    pact->workloads = calloc((size_t)config->nr_workloads, sizeof(pact_workload_t));
    if (!pact->workloads) {
        fprintf(stderr, "Error: Failed to allocate workload\n");
        return -1;
    }
    for (int w = 0; w < config->nr_workloads; w++) {
        pact->workloads[w].pidfd = -1;
    }

    int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
    bool *cpu_used = calloc(max_cpus, sizeof(bool));
    if (!cpu_used) {
        fprintf(stderr, "Error: Failed to allocate CPU tracking array\n");
        free(pact->workloads);
        pact->workloads = NULL;
        return -1;
    }

    /* PACT keeps one PAC table for every workload; Tierce one per workload. */
    bool tierce = pact->mode == PACT_MODE_TIERCE;
    pact->all_cpus_mask = 0;
    for (int w = 0; w < config->nr_workloads; w++) {
        pact->nr_workloads = w + 1;
        if (init_workload(&pact->workloads[w], &config->workloads[w], w, tierce ? w : 0, max_cpus,
                          cpu_used, &pact->all_cpus_mask) != 0) {
            cleanup_workloads(pact);
            free(cpu_used);
            return -1;
        }
    }

    int nr_domains = tierce ? config->nr_workloads : 1;
    pact->domains = calloc((size_t)nr_domains, sizeof(pac_domain_t));
    if (!pact->domains) {
        fprintf(stderr, "Error: Failed to allocate PAC table state\n");
        cleanup_workloads(pact);
        free(cpu_used);
        return -1;
    }
    pact->nr_domains = nr_domains;
    for (int d = 0; d < nr_domains; d++) {
        if (init_domain(&pact->domains[d], pact->mode) != 0) {
            cleanup_workloads(pact);
            free(cpu_used);
            return -1;
        }
    }

    if (build_workload_cpu_set(pact, max_cpus, cpu_used) != 0) {
        cleanup_workloads(pact);
        free(cpu_used);
        return -1;
    }

    free(cpu_used);
    return 0;
}

/* Every workload runs in its own cgroup with a finite fast-tier cap, and
 * the kernel enforces that cap: memory.mmctl_enabled turns the per-node
 * limits on, and memory.demotion runs the cgroup's kdemotiond, the only
 * thread that makes room under the cap. */
static int check_workload_cgroups(const pact_context_t *pact)
{
    static const char *const switches[] = {"memory.mmctl_enabled", "memory.demotion"};
    for (int w = 0; w < pact->nr_workloads; w++) {
        const pact_workload_t *wl = &pact->workloads[w];
        for (int s = 0; s < 2; s++) {
            int on = pact_cgroup_switch(wl->cgroup_path, switches[s]);
            if (on != 1) {
                log_error("pact_init",
                          "Workload %d: %s/%s %s; write \"enabled\" to it before the start", w,
                          wl->cgroup_path, switches[s], on == 0 ? "is disabled" : "cannot be read");
                return -1;
            }
        }
        uint64_t cap;
        int result = pact_cgroup_node0_cap(wl->cgroup_path, &cap);
        if (result == -1) {
            log_error("pact_init",
                      "Workload %d: cannot read %s/memory.max_at_node0; every workload needs its "
                      "own cgroup with a fast-tier cap",
                      w, wl->cgroup_path);
            return -1;
        }
        if (result < 0) {
            log_error("pact_init",
                      "Workload %d: %s/memory.max_at_node0 holds no finite cap (it reads \"max\", "
                      "the kernel's page-counter maximum, or no number); write the cap in bytes "
                      "before the start",
                      w, wl->cgroup_path);
            return -1;
        }
        printf("Workload %d cgroup %s: fast-tier cap %lu bytes\n", w, wl->cgroup_path, cap);
    }
    return 0;
}

static void copy_policy_and_intervals(pact_context_t *pact, const pact_config_t *config)
{
    pact->cooling_alpha = config->cooling_alpha;
    pact->cooling_trigger_samples = config->cooling_trigger_samples;
    pact->sampling_interval_ms = config->sampling_interval_ms;
    pact->cooling_interval_ms = config->cooling_interval_ms;
    pact->adaptive_interval_ms = config->adaptive_interval_ms;
    pact->stats_interval_ms = config->stats_interval_ms;
    pact->max_migrations_per_cycle = config->max_migrations_per_cycle;
    pact->domain_migration_batch = config->domain_migration_batch;
    pact->enable_logging = config->enable_logging;
}

static void init_migration_and_optimizations(pact_context_t *pact, const pact_config_t *config)
{
    atomic_init(&pact->migration_thread_running, false);
    pact->max_pac_entries = config->pac_pool_max;
    pact->monitor_cpu = config->monitor_cpu;
    pact->migration_cpu = config->migration_cpu;
}

/* Tierce: every workload CPU needs an entry in the platform's TID table,
 * which pairs the core with its CHA. There is no aggregate-MLP fallback. */
static int check_tid_table(const pact_context_t *pact)
{
    for (int w = 0; w < pact->nr_workloads; w++) {
        const pact_workload_t *wl = &pact->workloads[w];
        for (int c = 0; c < wl->nr_target_cpus; c++) {
            int cpu = wl->target_cpus[c];
            if (cpu >= g_pmu_platform.nr_cha_mapping) {
                log_error("pact_init",
                          "Workload %d: CPU %d has no entry in the %s TID table (CPUs 0-%d); "
                          "Tierce mode cannot measure its MLP",
                          w, cpu, g_pmu_platform.name, g_pmu_platform.nr_cha_mapping - 1);
                return -1;
            }
        }
    }
    return 0;
}

static int initialize_pact_context(pact_context_t *pact, const pact_config_t *config)
{
    pact->mode = config->mode;
    pact->pac_epoch = 1;
    pact->pebs_sampling_period = pact_config_pebs_period(config);
    pact->use_ldlat = config->use_ldlat;
    pact->ldlat_gate = pact_config_ldlat_gate(config);
    pact->ldlat_dyn.cfg = config->ldlat_dyn;
    pact->cat.cfg = config->cat;
    snprintf(pact->trace_migration_path, sizeof(pact->trace_migration_path), "%s",
             config->trace_migration);
#ifdef PACT_DIAG
    snprintf(pact->dump_pac_table_path, sizeof(pact->dump_pac_table_path), "%s",
             config->dump_pac_table);
    pact->dump_pac_interval_s =
        config->dump_pac_interval_s ? config->dump_pac_interval_s : PAC_DUMP_DEFAULT_INTERVAL_S;
    pact->no_promote_mask = config->no_promote_mask;
#endif
    if (initialize_workloads(pact, config) != 0) {
        return -1;
    }
    if (check_workload_cgroups(pact) != 0) {
        return -1;
    }

    copy_policy_and_intervals(pact, config);
    init_migration_and_optimizations(pact, config);

    if (config->enable_logging) {
        init_logging_wrapper(pact, config->log_format, config);
        for (int w = 0; w < pact->nr_workloads; w++) {
            log_workload_info(pact, "init", w, pact->workloads[w].target_pid,
                              pact->workloads[w].name, config->workloads[w].cpus);
        }
    }

    pact->sample_counts = 0;
    pact->bin_width = config->bin_width;
    pact->bin_count = config->bin_count;

    pact->tsc_freq_hz = pact_detect_tsc_frequency();
    printf("Detected TSC frequency: %.2f GHz\n", pact->tsc_freq_hz / 1e9);
    int platform = pmu_platform_init();
    if (platform < 0 && pact->mode == PACT_MODE_TIERCE) {
        /* The TID table of an unknown CPU is unknown: no override. */
        log_error("pact_init", "Unknown CPU platform; Tierce mode needs the platform's TID "
                               "table and does not accept PACT_ALLOW_UNKNOWN_PLATFORM");
        return -1;
    }
    if (platform < 0) {
        /*
         * Unknown CPU: the SKX-specific PMU event encodings, CHA-to-core
         * mapping, and k-constants would be programmed into an incompatible
         * PMU, producing plausible-but-invalid results. Abort by default.
         * Set PACT_ALLOW_UNKNOWN_PLATFORM=1 only if you know your CPU is
         * SKX-counter-compatible and accept the SKX fallback.
         */
        if (getenv("PACT_ALLOW_UNKNOWN_PLATFORM")) {
            log_warning("pact_init", "Unknown CPU platform; using SKX defaults "
                                     "(PACT_ALLOW_UNKNOWN_PLATFORM set): results may be invalid");
        } else {
            log_error("pact_init", "Unknown CPU platform. PACT's PMU events/constants are "
                                   "set for the supported Intel server CPUs; running on another "
                                   "CPU would yield invalid results. Set "
                                   "PACT_ALLOW_UNKNOWN_PLATFORM=1 to override at your own risk.");
            return -1;
        }
    }
    pact->k_constant_cxl = g_pmu_platform.k_constant_cxl;
    /* No MLP measurement exists yet: start at the platform's floor. */
    pact->global_mlp = g_pmu_platform.mlp_min;
    for (int w = 0; w < pact->nr_workloads; w++) {
        pact->workloads[w].mlp = g_pmu_platform.mlp_min;
        pact->workloads[w].k_slow = (double)pact->k_constant_cxl; /* LDLAT baseline start */
    }
    if (pact->mode == PACT_MODE_TIERCE && check_tid_table(pact) < 0) {
        return -1;
    }
    /* The events a CAT signal needs join the per-CPU groups, which open
     * after this. */
    if (pact->cat.cfg.enabled &&
        cat_signal_events(pact->cat.cfg.signal, pact->cat.cfg.trace_file[0] != '\0',
                          cat_gp_counters(), &pact->cat.events) < 0) {
        log_error("pact_init", "--cat-signal %s cannot run on this CPU",
                  cat_signal_name(pact->cat.cfg.signal));
        return -1;
    }
    printf("Mode: %s, %d workloads, %d PAC tables, %s period %lu\n", pact_mode_name(pact->mode),
           pact->nr_workloads, pact->nr_domains, pact->use_ldlat ? "LDLAT" : "PEBS",
           pact->pebs_sampling_period);
    if (pact->use_ldlat && pact->ldlat_dyn.cfg.enabled) {
        printf("LDLAT: floor %u cycles, software gate set by LDLAT-Dyn (open, or %u cycles when "
               "tight)\n",
               LDLAT_HW_THRESHOLD, pact->ldlat_dyn.cfg.tight);
    } else if (pact->use_ldlat && pact->ldlat_gate > LDLAT_HW_THRESHOLD) {
        printf("LDLAT: floor %u cycles, software gate %u cycles\n", LDLAT_HW_THRESHOLD,
               pact->ldlat_gate);
    } else if (pact->use_ldlat) {
        printf("LDLAT: floor %u cycles, no software gate\n", LDLAT_HW_THRESHOLD);
    }
    return 0;
}

/* Start the LDLAT-Dyn governor and schedule its first 1 s tick. */
static void start_ldlat_dyn(pact_context_t *pact)
{
    int result = ldlat_dyn_init(pact);
    if (result == 0) {
        pact->ldlat_dyn.next_tsc = rdtsc() + sec_to_tsc(pact, 1);
    } else if (result < 0) {
        log_warning("main", "LDLAT-Dyn cannot engage; the gate stays open");
    }
}

/* Start the LLC way controller after the per-CPU groups opened. Returns -1
 * when the run must stop: a way bound that does not fit the cache, or a
 * CLOS group of the controller that exists already. After any other start
 * failure the run continues without the controller, and its events leave
 * the foreground CPUs' counting groups. */
static int start_cat(pact_context_t *pact)
{
    if (!pact->cat.cfg.enabled) {
        return 0;
    }
    int result = cat_init(pact);
    if (result == 0) {
        pact->cat.next_tsc = rdtsc() + ms_to_tsc(pact, pact->cat.cfg.interval_ms);
        return 0;
    }
    if (result == -2) {
        log_error("main", "LLC way controller could not engage; exiting");
        return -1;
    }
    log_warning("main", "LLC way controller disabled (see the warnings above)");
    close_cat_counting_events(pact);
    return 0;
}

/* One pidfd per workload: a stable identity for exit detection. */
static int open_workload_pidfds(pact_context_t *pact)
{
    for (int w = 0; w < pact->nr_workloads; w++) {
        pact_workload_t *wl = &pact->workloads[w];
        wl->pidfd = syscall(SYS_pidfd_open, wl->target_pid, 0);
        if (wl->pidfd < 0) {
            log_error("main", "Cannot open the pidfd of workload %d (PID %d): %s", w,
                      (int)wl->target_pid, strerror(errno));
            return -1;
        }
    }
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
    if (pact_init(g_pact_ctx) < 0) {
        pact_destroy(g_pact_ctx);
        return 1;
    }
    start_ldlat_dyn(g_pact_ctx);

    printf("PACT initialized for %d workloads across %d CPUs\n", g_pact->nr_workloads,
           g_pact->nr_all_cpus);

    if (open_workload_pidfds(g_pact) < 0) {
        pact_destroy(g_pact);
        return 1;
    }
    pact_signal_install_handlers(&g_pact->running);
    if (setup_pact_perf_events(g_pact) < 0 || start_cat(g_pact) < 0) {
        pact_destroy(g_pact);
        return 1;
    }
    /* A shutdown signal during the start has no loop to stop yet: honor it
     * here instead of starting the event loop. */
    g_pact->running = !g_pact->sampling_failed && pact_signal_received() == 0;

    printf("=== PACT Runtime Mode: Coroutines and a Migration Thread ===\n");
    printf("Architecture: event loop with cooperative coroutines, migration in its own thread\n\n");

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
