/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* stats.c: comprehensive stats output at exit. */

#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "obj-pool.h"
#include "pact.h"
#include "stats.h"
#include "tsc.h"
#include "pebs-aggregator.h"

static void print_basic_stats(pact_context_t *pact)
{
    printf("\n=== PACT Statistics ===\n");
    printf("Mode: %s\n", pact_mode_name(pact->mode));
    pact_stats_t *quality = &pact->stats;
    uint64_t uncovered = 0;
    for (int w = 0; w < pact->nr_workloads; w++) {
        uncovered += pact->workloads[w].stats.pebs_uncovered;
    }
    printf("Sampling quality: lost=%lu loss_records=%lu throttles=%lu malformed=%lu "
           "overruns=%lu uncovered=%lu update_drops=%lu\n",
           quality->pebs_lost_samples, quality->pebs_lost_records, quality->pebs_throttles,
           quality->pebs_malformed, quality->pebs_overruns, uncovered, quality->pebs_update_drops);
    printf("Node queries before promotion calls that failed: %lu\n",
           (uint64_t)atomic_load(&pact->residency_query_failures));
}

static void print_workload_stats(pact_context_t *pact, pact_workload_t *wl)
{
    pact_stats_t *st = &wl->stats;
    printf("\n=== Workload %d (%s, PID %d) ===\n", wl->index, wl->name, (int)wl->target_pid);
    if (pact->use_ldlat) {
        printf("LDLAT: k_slow=%.1f cycles, not remote DRAM=%lu, below gate=%lu\n", wl->k_slow,
               wl->ldlat_filtered, wl->ldlat_gated);
    }
    printf("Counter running time (summed %s ns): %lu\n",
           pact->mode == PACT_MODE_TIERCE ? "CPU" : "task", st->time_running);
    printf("LLC Misses (slow): %lu\n", st->llc_misses_slow);
    printf("PAC Updates: %lu\n", st->pac_updates);
    printf("PEBS samples: %lu (uncovered %lu)\n", wl->total_pebs_samples, st->pebs_uncovered);
    printf("Promotions (successful): %lu\n", st->promotion_successes);
    printf("Demotions (cgroup): %lu\n", (uint64_t)atomic_load(&st->demotion_successes));

    printf("Total Repromotions (0→1→0): %lu\n", st->total_repromotions);
    printf("Total Redemotions (1→0→1): %lu\n", st->total_redemotions);
    printf("Total Ping-Pong Pages: %lu\n", st->total_repromotions + st->total_redemotions);

    printf("Promotion Attempts: %lu\n", st->promotion_attempts);
    printf("Promotion Successes: %lu\n", st->promotion_successes);
    printf("Promotion Failures: %lu\n", st->promotion_failures);
    printf("Promotion Skips (already on the fast tier): %lu\n",
           (uint64_t)atomic_load(&st->promotion_skips_resident));
    if (st->promotion_attempts > 0) {
        double rate = (double)st->promotion_successes / st->promotion_attempts * 100.0;
        printf("Promotion Success Rate: %.1f%%\n", rate);
    }
}

static void print_hash_table_stats(pact_context_t *pact)
{
    printf("\n=== PAC Tables ===\n");
    printf("Cooling Decays: %lu\n", pact->stats.cooling_decays);
    printf("Avg PAC Value: %lu\n", pact->stats.avg_pac);
    uint64_t entries = 0;
    for (int d = 0; d < pact->nr_domains; d++) {
        entries += kh_size(pact->domains[d].table);
    }
    printf("Hash table entries: %lu\n", entries);
    for (int d = 0; d < pact->nr_domains; d++) {
        pac_domain_t *domain = &pact->domains[d];
        binning_state_t *bin = domain->binning;
        if (pact->mode == PACT_MODE_TIERCE) {
            printf("  Table %d: hash=%u, bar=%.2f, ring_full_drops=%lu, list_full_drops=%lu, "
                   "no_bar_epochs=%lu, node_query_failures=%lu\n",
                   d, kh_size(domain->table), domain->bar, domain->ring_full_drops,
                   domain->list_full_drops, domain->no_bar_epochs,
                   domain->pages ? domain->pages->query_failures : 0);
            continue;
        }
        printf("  Table %d: hash=%u, Q1=%.2f, Q3=%.2f, bw=%.2f, ring_full_drops=%lu\n", d,
               kh_size(domain->table), bin ? bin->q1 : 0.0, bin ? bin->q3 : 0.0,
               bin ? bin->bin_width : 0.0, domain->ring_full_drops);
    }
}

static void print_mlp_and_policy(pact_context_t *pact)
{
    if (pact->mode == PACT_MODE_TIERCE) {
        for (int w = 0; w < pact->nr_workloads; w++) {
            printf("Workload %d MLP: slow %.2f\n", w, pact->workloads[w].mlp);
        }
    } else {
        printf("Global MLP: slow %.2f\n", pact->global_mlp);
    }
    printf("Cooling: alpha=%.2f, trigger=%lu samples per table\n", pact->cooling_alpha,
           pact->cooling_trigger_samples);
    const cat_state_t *cat = &pact->cat;
    if (cat->enabled) {
        printf("CAT: signal=%s W_fg=%d range=[%d,%d] ticks=%lu reallocations=%lu\n",
               cat_signal_name(cat->signal), cat->w_fg, cat->w_min, cat->w_max, cat->ticks,
               cat->reallocations);
    }
    const ldlat_dyn_state_t *dyn = &pact->ldlat_dyn;
    if (dyn->enabled) {
        printf("LDLAT-Dyn: mode=%s gate=%u pressure=%.5f ticks=%lu mode_changes=%lu "
               "admitted=%lu\n",
               dyn->mode == LDLAT_DYN_TIGHT ? "TIGHT" : "OPEN", dyn->gate, dyn->pressure,
               dyn->ticks, dyn->mode_changes, dyn->admitted);
    }
}

static void print_pool_stats(pact_context_t *pact)
{
    if (!pact->pac_metadata_pool) {
        return;
    }
    size_t cap = get_total_capacity(pact->pac_metadata_pool);
    size_t avail = pool_available(pact->pac_metadata_pool);
    size_t in_use = cap - avail;

    printf("\n=== Memory Pool Statistics ===\n");
    printf("PAC Metadata Pool:\n");
    printf("  Total Capacity: %zu objects\n", cap);
    printf("  Available Objects: %zu\n", avail);
    printf("  Objects In Use: %zu\n", in_use);
    printf("  Total Memory: %zu bytes\n", get_total_memory(pact->pac_metadata_pool));
    printf("  Element Size: %zu bytes\n", get_element_size(pact->pac_metadata_pool));
    printf("  Pool Usage: %.1f%%\n", cap > 0 ? (double)in_use / cap * 100.0 : 0.0);
    if (pact->max_pac_entries > 0) {
        printf("  Max Entries Cap: %zu per table\n", pact->max_pac_entries);
    }
    printf("  Alloc Skipped (pool full): %lu\n", pact->stats.pool_alloc_skipped);
}

/* Coroutine scheduling stats: per-coroutine resume count + late-resume tally. */
static void print_one_coro_sched(pact_context_t *pact, const char *name, coro_type_t type)
{
    const struct coro_timing *t = &pact->timing[type];
    uint64_t avg_late_ms = (t->late_counts > 0) ? tsc_to_ms(pact, t->late_tsc / t->late_counts) : 0;
    printf("%s Coroutine: %lu resumes, %lu late resumes, avg %lu ms late\n", name, t->counts,
           t->late_counts, avg_late_ms);
}

static void print_coro_sched_stats(pact_context_t *pact)
{
    printf("\n=== Coroutine Scheduling Statistics ===\n");
    /* Display names retained for human-readable stats output ("Sampling"
     * rather than the internal "PEBS"). The adaptive coroutine is not
     * listed. */
    print_one_coro_sched(pact, "Sampling", CORO_TYPE_PEBS);
    print_one_coro_sched(pact, "Cooling", CORO_TYPE_COOLING);
    print_one_coro_sched(pact, "Stats", CORO_TYPE_STATS);
}

void print_stats(pact_context_t *pact)
{
    print_basic_stats(pact);
    for (int w = 0; w < pact->nr_workloads; w++) {
        print_workload_stats(pact, &pact->workloads[w]);
    }
    print_hash_table_stats(pact);
    print_mlp_and_policy(pact);
    print_pool_stats(pact);
    print_coro_sched_stats(pact);
}
