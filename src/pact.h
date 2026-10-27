/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/*
 * pact.h: PACT runtime context, workload state, and PEBS sample encoding.
 */

#ifndef PACT_H
#define PACT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include "pmu.h"
#include "obj-pool.h"
#include "khashl.h"
#include "ring-buffer.h"
#include "ldlat-dyn.h"
#include "cat.h"
#include "trace.h"
#include "diag.h"

/* Mark a function parameter as deliberately unused (e.g. only referenced in
 * one branch of an #ifdef). Attaches the attribute at the parameter so no
 * `(void)param;` statements are needed in the body. */
#ifndef PACT_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define PACT_UNUSED __attribute__((unused))
#else
#define PACT_UNUSED
#endif
#endif

/* Forward declarations */
typedef struct mco_coro mco_coro;
/* pact_context_t is forward-declared in pmu.h (included above) */

/* Most workloads one runtime instance manages (--workload is repeatable). */
#define MAX_WORKLOADS 16

/* Runtime mode (--mode). PACT keeps one PAC table, one threshold and one
 * migration queue for every workload. Tierce keeps one of each per workload
 * (a domain), with per-workload MLP. */
typedef enum {
    PACT_MODE_PACT = 0,
    PACT_MODE_TIERCE = 1,
} pact_mode_t;

static inline const char *pact_mode_name(pact_mode_t mode)
{
    return mode == PACT_MODE_TIERCE ? "tierce" : "pact";
}

/*
 * PAC metadata. The tier/flag bytes are updated concurrently by the
 * sampling path and the migration thread; each is an _Atomic byte so a
 * store of one field never changes another field. Both threads store
 * tier, so the last store wins.
 */
typedef struct pac_metadata {
    uint64_t page_addr;    /* Page virtual address */
    pid_t pid;             /* Owner process ID */
    uint64_t pac_value;    /* Accumulated PAC score */
    uint32_t access_count; /* Access frequency */
    uint32_t epoch_stamp;  /* Tierce: last epoch that listed the page (0 = never) */
    uint8_t workload;      /* Number of the owner workload (--workload order) */

    _Atomic uint8_t tier;      /* Current memory tier */
    _Atomic uint8_t prev_tier; /* Previous tier (ping-pong detection) */
    _Atomic uint8_t migrating; /* Migration in progress */
} pac_metadata_t;

/* Hash map type declarations - must be here for khash_t(pac) to work */
KHASHL_MAP_INIT(KH_LOCAL, pac_table_t, pac_table, uint64_t, pac_metadata_t *, kh_hash_uint64,
                kh_eq_generic)

/* PAC table key: (workload number, page address). Page addresses use bits
 * 12-56 (57-bit virtual addresses), so the workload number fits above. */
_Static_assert(MAX_WORKLOADS <= 128, "workload number must fit in key bits 57-63");
static inline uint64_t pac_key(unsigned int workload, uint64_t page_addr)
{
    return page_addr | ((uint64_t)workload << 57);
}

/* Coroutine types */
typedef enum {
    CORO_TYPE_PEBS,    /* sampling */
    CORO_TYPE_PAC,     /* adaptive */
    CORO_TYPE_COOLING, /* cooling */
    CORO_TYPE_STATS,   /* stats */
    CORO_TYPE_MAX
} coro_type_t;

/*
 * Single page migration entry for thread-based migration
 * Passed through ring buffer from main thread to migration thread
 */
typedef struct migration_entry {
    pac_metadata_t *meta; /* Page metadata pointer (contains pid, page_addr) */
    int target_node;      /* Target NUMA node (0=fast, 1=slow) */
} migration_entry_t;

/* PEBS event encoding: assume 57 bits virtual addr, we use:
 * - 63-57, 7 bits: pac value (high)
 * - 56-12, 45 bits: page address (page-aligned)
 * - 11-1, 11 bits: pac value (low)
 * - 0, 1 bit: tier (0=fast, 1=slow)
 * Total: 7+11=18 bits for PAC value, max 262143. Per-sample stalls are
 * ~k_cxl*period/MLP (~308K at period 400 and MLP 1), so 18 bits saturate
 * only when the slow-tier MLP is close to 1. */
#define PEBS_ENCODE_PAC(pac, page_addr, tier)                                                      \
    ({                                                                                             \
        uint32_t _pac = (uint32_t)(pac);                                                           \
        uint64_t _addr = ((uint64_t)(page_addr)) & PAGE_MASK;                                      \
        uint64_t _tier = (uint64_t)(tier) & 0x1ULL; /* 1 bit */                                    \
                                                                                                   \
        uint64_t _pac_high = (_pac >> 11) & 0x7FULL; /* 7 bits */                                  \
        uint64_t _pac_low = _pac & 0x7FFULL;         /* 11 bits */                                 \
                                                                                                   \
        ((_pac_high << 57) | ((_addr >> PAGE_SHIFT) << 12) | /* Bits 56-12: page address */        \
         (_pac_low << 1) |                                   /* Bits 11-1: pac_low */              \
         _tier);                                             /* Bit 0: tier */                     \
    })

#define PEBS_DECODE_PAC(encoded)                                                                   \
    ({                                                                                             \
        uint32_t _pac_high = ((encoded) >> 57) & 0x7FULL; /* 7 bits */                             \
        uint32_t _pac_low = ((encoded) >> 1) & 0x7FFULL;  /* 11 bits */                            \
        (_pac_high << 11) | _pac_low;                                                              \
    })

/* PEBS address encoding: low 3 bits for metadata (addresses are page-aligned) */
#define PEBS_ENCODE_ADDR_TIER(addr, tier)                                                          \
    (((addr) & PAGE_MASK) | /* Preserve page address */                                            \
     ((tier) & 0x1ULL)      /* Bit 0: tier */                                                      \
    )

#define PEBS_DECODE_ADDR(encoded) ((encoded) & ~0x7ULL) /* Clear bits 2-0 */
#define PEBS_DECODE_PAGE(encoded) ((encoded) & PAGE_MASK)
#define PEBS_DECODE_TIER(encoded) ((encoded) & 0x1ULL)

/* Fast PRNG for reservoir sampling */
typedef struct {
    uint64_t state;
} fast_prng_t;

/* Reservoir sampling state */
typedef struct {
    double *samples;
    size_t capacity;
    size_t count;
    uint64_t total_seen;
    fast_prng_t prng;
} reservoir_t;

/* The kernel's node of each of `count` pages of process `pid`, written to
 * `status` (a negative errno for a page without a node). Returns a negative
 * value when the query fails as a whole. */
typedef long (*page_node_query_fn)(int pid, unsigned long count, void **pages, int *status);

/* Tierce page reservoir: a uniform sample of the pages of one PAC table.
 * A page joins when it enters the table (classic reservoir rule); pages
 * leave the table only at exit, so the member pointers stay valid. */
typedef struct {
    pac_metadata_t **members;
    double *values; /* Scratch for the quantile of the members' PAC values */
    /* Scratch for the node query of the members held on the fast tier */
    void **query_pages;
    int *query_status;
    size_t *query_members;
    page_node_query_fn query_nodes;
    uint64_t query_failures; /* Node queries that failed as a whole */
    size_t capacity;
    size_t count;
    uint64_t total_seen;
    fast_prng_t prng;
} page_reservoir_t;

/* Binning state for adaptive thresholds */
typedef struct {
    double bin_width;
    size_t bin_count;
    double q1, q3; /* Quartiles */
    uint64_t last_update;
} binning_state_t;

/* Comprehensive statistics structure */
typedef struct {
    /* Migration statistics */
    /* Written with atomic RMW, promotion_attempts by the event loop and the
     * others by the migration thread. Read by the stats coroutine, the
     * migration trace and LDLAT-Dyn. _Atomic makes those cross-thread reads
     * defined. */
    _Atomic uint64_t promotion_attempts;
    _Atomic uint64_t promotion_successes;
    _Atomic uint64_t promotion_failures;
    /* Queued pages that the node query before the call found already on
     * the fast tier: not sent, not promotions. */
    _Atomic uint64_t promotion_skips_resident;
    /* Kernel LRU demotions this run (owned by balance.c on the migration
     * thread: pgdemote deltas relative to the startup baseline). The stats
     * coroutine reads it on the event-loop thread, so it is _Atomic. */
    _Atomic uint64_t demotion_successes;

    /* PAC and memory access statistics */
    uint64_t time_running; /* Sum of per-task counter running nanoseconds. */
    uint64_t llc_misses_slow;
    uint64_t pac_updates;
    uint64_t avg_pac;

    /* Cooling statistics */
    uint64_t cooling_decays;

    /* Cumulative PEBS quality diagnostics, owned by the sampling coroutine. */
    uint64_t pebs_lost_samples;
    uint64_t pebs_lost_records;
    uint64_t pebs_throttles;
    uint64_t pebs_malformed;
    uint64_t pebs_overruns;
    uint64_t pebs_uncovered;
    uint64_t pebs_update_drops;

    /* PEBS sampling */
    uint64_t pebs_events_processed;

    /* Simple ping-pong tracking (tier history-based) */
    uint64_t total_repromotions; /* Total 0→1→0 patterns */
    uint64_t total_redemotions;  /* Total 1→0→1 patterns */

    /* Pool capacity tracking */
    uint64_t pool_alloc_skipped; /* Pages skipped because PAC metadata pool is full */
    uint64_t pool_warn_last_tsc; /* TSC of last "pool full" log warning (rate-limit) */
} pact_stats_t;

/* One entry of the PAC update ring (PEBS aggregator -> adaptive coroutine):
 * the 64-bit PEBS_ENCODE_PAC word (18-bit PAC, page address, tier) and the
 * number of the workload that owns the page, in its own field. */
typedef struct pac_update {
    uint64_t encoded;
    uint32_t workload;
    uint32_t reserved;
} pac_update_t;

/* Workload number of the entry that ends a Tierce epoch: the sampling
 * window's updates before it are applied, then each domain refreshes its
 * bar and admits its pages. */
#define PAC_UPDATE_EPOCH_END UINT32_MAX

DEFINE_RING_BUFFER(pac_update_t, pac_update)

/* Ring buffer for migration entries (thread-based migration, single-page) */
DEFINE_RING_BUFFER(migration_entry_t, migration_entry)

/* Per-coroutine scheduling + lateness state. Indexed by enum coro_type so
 * adding a new coroutine costs one enum entry, not five new field names. */
struct coro_timing {
    uint64_t next_tsc;    /* deadline: when this coroutine should next fire */
    uint64_t prev_tsc;    /* TSC at the start of its most recent tick */
    uint64_t late_tsc;    /* cumulative late-by (lag past deadline) */
    uint64_t late_counts; /* # of late ticks */
    uint64_t counts;      /* total # of ticks */
};

/*
 * One PAC table with its admission state and migration queue. PACT keeps
 * one table for every workload: the score distribution, the threshold and
 * the queue are global. Tierce keeps one table per workload (a domain).
 */
typedef struct pac_domain {
    pac_table_t *table;
    size_t nr_metadata; /* Metadata entries allocated for this table */

    /* PACT adaptive binning: a sample of PAC values and the bin state. */
    reservoir_t *reservoir;
    binning_state_t *binning;

    /* Tierce admission: the page reservoir, the 0.90-quantile bar of the
     * members' PAC values, and the slow-tier pages updated in this epoch
     * (each listed once). */
    page_reservoir_t *pages;
    double bar;
    pac_metadata_t **epoch_pages;
    size_t nr_epoch_pages;
    size_t epoch_pages_cap;
    uint64_t epoch_updates;   /* PAC updates of this table in this epoch */
    uint64_t no_bar_epochs;   /* Epochs with PAC updates but no bar, so no admission */
    uint64_t list_full_drops; /* Pages not listed because the list could not grow */

    /* Pages dropped at a full migration ring: admitted pages (Tierce) or
     * top-bin pages (PACT). */
    uint64_t ring_full_drops;

    ring_buffer_migration_entry_t *migration_ring;

    /* Cooling trigger: samples applied to this table, and the count at the
     * last cooling. */
    uint64_t samples;
    uint64_t cooling_last_trigger;
} pac_domain_t;

/*
 * Per-workload structure
 * Holds workload-specific configuration, PMU state, and statistics
 */
typedef struct pact_workload {
    /* Process identification */
    pid_t target_pid;      /* Process ID */
    char name[64];         /* Workload name/label */
    char cgroup_path[256]; /* The workload's cgroup directory */
    int index;             /* Workload number (--workload order) */
    int domain;            /* Index of the PAC table that holds its pages */
    int pidfd;             /* Stable workload identity and exit readiness */
    _Atomic bool exited;   /* The process has exited; it is no longer sampled */

    /* CPU configuration */
    int *target_cpus;   /* Array of CPU IDs this workload runs on */
    int nr_target_cpus; /* Number of CPUs */
    uint64_t cpu_mask;  /* Bitmask of CPUs (for quick checks) */

    /* Non-inheriting groups for each observed task, refreshed between windows. */
    thread_perf_t *threads;
    size_t nr_threads;

    /* Demotion statistics: the cgroup's pgdemote count at startup. */
    uint64_t demotion_baseline;

    /* Tierce: slow-tier MLP of the window, the mean of the per-core MLP
     * over every CPU of the workload (wMLP). */
    double mlp;

    /* Tierce LDLAT: slow-tier baseline latency (cycles), an EWMA of the
     * mean latency of the workload's samples that pass the data-source
     * filter, before any software gate. A sample adds S_p =
     * S_w * latency / k_slow to PAC, with k_slow from before the window's update. */
    double k_slow;
    uint64_t ldlat_filtered; /* samples not from remote DRAM (data-source filter) */
    uint64_t ldlat_gated;    /* samples below the software latency gate */

    /* Per-workload statistics */
    pact_stats_t stats;          /* Workload-specific statistics */
    uint64_t total_pebs_samples; /* Cumulative PEBS samples attributed to this workload */
} pact_workload_t;

/* PACT runtime context: one instance per process. It owns the workloads. */
struct pact_context {
    pact_mode_t mode;

    /* ===== Core Data Structures ===== */
    ring_buffer_pac_update_t *pac_update_ring; /* PEBS -> PAC updates ring buffer */

    /* The workloads, in --workload order: owned by the context. */
    pact_workload_t *workloads;
    int nr_workloads;

    /* PAC tables: PACT keeps one for every workload, Tierce one per
     * workload. */
    pac_domain_t *domains;
    int nr_domains;

    /* Tierce epoch number; a page listed in this epoch carries it. */
    uint32_t pac_epoch;

    /* CHA slices for the MLP. PACT: every mapped CHA, TID filter off.
     * Tierce: the CHA paired with each workload CPU, TID filter on. */
    cha_pmu_info_t cha_pmus[MAX_CHAS];
    int nr_cha;
    /* Slow-tier MLP of the current window: one ratio of sums over every
     * fresh CHA, shared by all workloads (Algorithm 1). */
    double global_mlp;

    /* Union of the workloads' CPU sets (capped to CPUs 0-63). */
    uint64_t all_cpus_mask;
    int *all_cpus;
    int nr_all_cpus;

    /* ===== Per-CPU States ===== */
    per_cpu_state_t *cpu_states; /* Array of per-CPU PMU states for all workload CPUs */
    int nr_cpus;                 /* Number of CPUs in the system */

    /* ===== Coroutine Management ===== */
    mco_coro *coroutines[CORO_TYPE_MAX]; /* Coroutine array */

    /* Coroutine scheduling timestamps, indexed by coro_type_t. */
    struct coro_timing timing[CORO_TYPE_MAX];
    uint64_t start_tsc;

    /* Deadline (TSC) for the next 1Hz check_targets_alive() tick. Lives on
     * the context so the event loop can fold it into next_deadline_tsc(). */
    uint64_t targets_alive_next_tsc;

    /* ===== Adaptive Components: initial config. The live state lives
     * on the PAC domain. ===== */
    double bin_width;
    size_t bin_count;

    /* ===== PEBS and PMU ===== */
    struct pebs_aggregator *pebs_aggregator; /* PEBS aggregator */
    bool pebs_available;                     /* PEBS availability flag */

    /* ===== Configuration ===== */
    /* Timing intervals (ms) */
    uint32_t sampling_interval_ms;
    uint32_t cooling_interval_ms;
    uint32_t adaptive_interval_ms;
    uint32_t stats_interval_ms;

    /* PEBS sampling period (of the open event: LLC-miss loads or LDLAT) */
    uint64_t pebs_sampling_period;

    /* Tierce LDLAT */
    bool use_ldlat;
    uint32_t ldlat_gate;         /* static software gate in cycles; 350 (the floor) = none */
    ldlat_dyn_state_t ldlat_dyn; /* LDLAT-Dyn governor (--ldlat-dyn) */

    /* Tierce LLC way controller (--use-cat) */
    cat_state_t cat;

    /* Per-second migration trace (--trace-migration) */
    migration_trace_t migration_trace;
    char trace_migration_path[256];

#ifdef PACT_DIAG
    pac_dump_t pac_dump; /* --dump-pac-table */
    char dump_pac_table_path[256];
    uint32_t dump_pac_interval_s;
    uint32_t no_promote_mask; /* --no-promote-wl: bit w keeps workload w out of promotion */
#endif

    uint32_t max_migrations_per_cycle; /* PACT: most pages per numa_move_pages call */
    uint32_t domain_migration_batch;   /* Tierce: most pages per domain per turn */

    /* Cooling factor + trigger (Algorithm 1 α-decay). */
    double cooling_alpha;             /* 1.0 = no cooling (default) */
    uint64_t cooling_trigger_samples; /* samples per PAC table */

    /* Cooling-related stats */
    uint64_t sample_counts;

    /* Hardware constants */
    uint64_t k_constant_cxl; /* CXL latency constant (771) */
    uint64_t tsc_freq_hz;    /* TSC frequency for time calculations */

    /* ===== Logging ===== */
#ifdef PACT_ENABLE_LOGGING
    bool enable_logging;
    FILE *log_file;
    char log_format[16]; /* "csv" */
#else
    bool enable_logging; /* Set by the CLI. Only the logging build reads it. */
#endif

    /* ===== Memory Pools ===== */
    object_pool *pac_metadata_pool; /* Object pool for metadata */
    size_t max_pac_entries;         /* Cap on PAC metadata entries (0 = unlimited) */

    /* ===== Control Flags ===== */
    /* Cleared by the event loop and by the shutdown signal handler, so it
     * has the type that a signal handler may write. */
    volatile sig_atomic_t running;
    bool sampling_failed;

    /* Run-wide statistics: PEBS buffer quality, PAC table capacity and
     * cooling. Per-workload counters live on each workload. */
    pact_stats_t stats;

    /* ===== Coroutine scheduling state ===== */

    /* ===== Migration Thread ===== */
    pthread_t migration_thread; /* Migration thread handle */
    /* Shared stop flag; volatile cannot synchronize threads. */
    _Atomic bool migration_thread_running;
    int migration_wake_fd;
    _Atomic bool migration_wake_pending;
    _Atomic int migration_error;
    /* Node queries before a promotion call that failed as a whole. */
    _Atomic uint64_t residency_query_failures;

    /* CPU affinity configuration */
    int monitor_cpu;   /* CPU for main event loop (-1 = no pinning) */
    int migration_cpu; /* CPU for migration thread (-1 = no pinning) */
};

/* Global context pointer */
extern pact_context_t *g_pact_ctx;

/* Short name of the global context pointer */
#define g_pact g_pact_ctx


/* TGID filter: the number of the workload whose target_pid is pid, or -1.
 * Exited workloads keep their slot, so their late samples still match. */
static inline int pact_find_workload(const pact_context_t *ctx, pid_t pid)
{
    for (int i = 0; i < ctx->nr_workloads; i++) {
        if (ctx->workloads[i].target_pid == pid) {
            return i;
        }
    }
    return -1;
}

/* Interval (in ms) for the given coroutine type. Looks up the configured
 * value from the per-subsystem config field. */
static inline uint32_t pact_coro_interval_ms(const pact_context_t *ctx, coro_type_t type)
{
    switch (type) {
    case CORO_TYPE_PEBS:
        return ctx->sampling_interval_ms;
    case CORO_TYPE_PAC:
        return ctx->adaptive_interval_ms;
    case CORO_TYPE_COOLING:
        return ctx->cooling_interval_ms;
    case CORO_TYPE_STATS:
        return ctx->stats_interval_ms;
    default:
        return 0;
    }
}

/* Human-readable name for a coroutine type. Used in log messages and the
 * stats summary. */
static inline const char *pact_coro_name(coro_type_t type)
{
    switch (type) {
    case CORO_TYPE_PEBS:
        return "PEBS";
    case CORO_TYPE_PAC:
        return "Adaptive";
    case CORO_TYPE_COOLING:
        return "Cooling";
    case CORO_TYPE_STATS:
        return "Stats";
    default:
        return "?";
    }
}

#endif /* PACT_H */
