/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* stats-coro.c: periodic stats coroutine.
 *
 * Runs every `stats_interval_ms`: logs the event and migration rates, a
 * summary line per workload and the PAC telemetry at INFO level (print_stats
 * runs at exit only). Uses minicoro coroutines.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "minicoro.h"
#include "pact.h"
#include "stats.h"
#include "stats-coro.h"
#include "tsc.h"
#include "pebs-aggregator.h"

static uint32_t total_hash_entries(pact_context_t *ctx)
{
    uint32_t entries = 0;
    for (int d = 0; d < ctx->nr_domains; d++) {
        entries += kh_size(ctx->domains[d].table);
    }
    return entries;
}

/* One line per workload; hash, mig_ring, Q1, Q3 and bw describe the PAC
 * table that holds the workload's pages (PACT: the shared table; Tierce:
 * the workload's own table, with its bar). */
static void log_workload_summary(pact_context_t *ctx)
{
    for (int w = 0; w < ctx->nr_workloads; w++) {
        pact_workload_t *wl = &ctx->workloads[w];
        pac_domain_t *domain = &ctx->domains[wl->domain];
        binning_state_t *bin = domain->binning;
        size_t ring =
            domain->migration_ring ? ring_buffer_migration_entry_size(domain->migration_ring) : 0;
        if (ctx->mode == PACT_MODE_TIERCE) {
            log_info("stats_coroutine",
                     "  WL%d (%s): hash=%u, mig_ring=%zu, bar=%.1f, ring_full_drops=%lu, "
                     "pebs_samples=%lu, no_bar_epochs=%lu",
                     w, wl->name, kh_size(domain->table), ring, domain->bar,
                     domain->ring_full_drops, wl->total_pebs_samples, domain->no_bar_epochs);
            continue;
        }
        log_info("stats_coroutine",
                 "  WL%d (%s): hash=%u, mig_ring=%zu, Q1=%.1f, Q3=%.1f, bw=%.1f, "
                 "pebs_samples=%lu, ring_full_drops=%lu",
                 w, wl->name, kh_size(domain->table), ring, bin ? bin->q1 : 0.0,
                 bin ? bin->q3 : 0.0, bin ? bin->bin_width : 0.0, wl->total_pebs_samples,
                 domain->ring_full_drops);
    }
}

/* A page meets the table's admission threshold: PACT's top-bin test, or
 * Tierce's bar. */
static bool meets_threshold(const pac_domain_t *domain, uint64_t pv)
{
    const binning_state_t *bin = domain->binning;
    if (!bin) {
        return (double)pv >= domain->bar;
    }
    /* Match the runtime's bin-index decision, including fractional widths. */
    return !(bin->bin_width > 0) || (double)pv / bin->bin_width >= (double)(bin->bin_count - 1);
}

/* PAC histogram for one PAC table: min/avg/max + per-tier averages + count
 * over threshold. Samples up to ~10K entries from large tables. */
static void log_one_domain_pac_dist(pac_domain_t *domain, const char *label)
{
    pac_table_t *table = domain->table;
    uint32_t table_size = kh_size(table);
    if (table_size == 0) {
        return;
    }

    binning_state_t *bin = domain->binning;
    /* Keep the displayed threshold floating-point: accepted widths may
     * produce a threshold beyond uint64_t without admitting any page. */
    double threshold = bin ? bin->bin_width * (bin->bin_count - 1) : domain->bar;
    uint64_t pac_min = UINT64_MAX, pac_max = 0;
    /* Up to uint32_t entries contribute uint64_t scores. Keep the sum wide
     * so the reported average remains between the sampled minimum/maximum. */
    __uint128_t pac_sum = 0, pac_fast_sum = 0, pac_slow_sum = 0;
    uint32_t n_total = 0, n_fast = 0, n_slow = 0, n_above_threshold = 0;
    uint32_t step = (table_size > 10000) ? table_size / 10000 : 1;
    uint32_t count = 0;

    for (khint_t k = 0; k != kh_end(table); k++) {
        if (!kh_exist(table, k)) {
            continue;
        }
        count++;
        if (step > 1 && (count % step) != 0) {
            continue;
        }
        pac_metadata_t *meta = kh_val(table, k);
        if (!meta) {
            continue;
        }
        uint64_t pv = meta->pac_value;
        n_total++;
        if (pv < pac_min) {
            pac_min = pv;
        }
        if (pv > pac_max) {
            pac_max = pv;
        }
        pac_sum += pv;
        if (meta->tier == 0) {
            n_fast++;
            pac_fast_sum += pv;
        } else {
            n_slow++;
            pac_slow_sum += pv;
        }
        if (meets_threshold(domain, pv)) {
            n_above_threshold++;
        }
    }

    if (n_total == 0) {
        return;
    }
    log_info("stats_coroutine",
             "  %s PAC_DIST: n=%u min=%lu avg=%lu max=%lu threshold=%.17g above_thresh=%u "
             "fast(n=%u avg=%lu) slow(n=%u avg=%lu)",
             label, n_total, pac_min, (uint64_t)(pac_sum / n_total), pac_max, threshold,
             n_above_threshold, n_fast, n_fast > 0 ? (uint64_t)(pac_fast_sum / n_fast) : 0, n_slow,
             n_slow > 0 ? (uint64_t)(pac_slow_sum / n_slow) : 0);
}

/* PACT scores every sample with one global scalar: the LLC misses of every
 * workload over the global MLP. */
static void log_pac_telemetry(pact_context_t *ctx)
{
    if (ctx->mode == PACT_MODE_TIERCE) {
        /* Tierce scores each workload's samples with its own scalar. */
        for (int w = 0; w < ctx->nr_workloads; w++) {
            pact_workload_t *wl = &ctx->workloads[w];
            char label[16];
            snprintf(label, sizeof(label), "WL%d", w);
            log_info("stats_coroutine",
                     "  %s PAC_INPUTS: llc_slow=%lu mlp_slow=%.2f k_cxl=%lu pebs_period=%lu", label,
                     wl->stats.llc_misses_slow, wl->mlp, ctx->k_constant_cxl,
                     ctx->pebs_sampling_period);
            if (ctx->use_ldlat) {
                /* Under LDLAT-Dyn the governor's gate is the gate in use; its
                 * open mode keeps every sample above the hardware floor. */
                uint32_t gate = ctx->ldlat_gate;
                if (ctx->ldlat_dyn.enabled) {
                    gate = ctx->ldlat_dyn.gate ? ctx->ldlat_dyn.gate : LDLAT_HW_THRESHOLD;
                }
                log_info("stats_coroutine",
                         "  %s LDLAT: k_slow=%.1f gate=%u not_remote=%lu below_gate=%lu", label,
                         wl->k_slow, gate, wl->ldlat_filtered, wl->ldlat_gated);
            }
            log_one_domain_pac_dist(&ctx->domains[wl->domain], label);
        }
        return;
    }
    uint64_t llc_misses = 0;
    for (int w = 0; w < ctx->nr_workloads; w++) {
        llc_misses += ctx->workloads[w].stats.llc_misses_slow;
    }
    log_info("stats_coroutine",
             "  WL PAC_INPUTS: llc_slow=%lu mlp_slow=%.2f k_cxl=%lu pebs_period=%lu", llc_misses,
             ctx->global_mlp, ctx->k_constant_cxl, ctx->pebs_sampling_period);
    log_one_domain_pac_dist(&ctx->domains[0], "WL");
}

/* Run-wide rates sum the per-workload counters. */
static void sum_workload_counters(pact_context_t *ctx, uint64_t *events, uint64_t *promotions,
                                  uint64_t *demotions)
{
    *events = *promotions = *demotions = 0;
    for (int w = 0; w < ctx->nr_workloads; w++) {
        pact_stats_t *st = &ctx->workloads[w].stats;
        *events += st->pebs_events_processed;
        *promotions += st->promotion_successes;
        *demotions += atomic_load_explicit(&st->demotion_successes, memory_order_relaxed);
    }
}

void stats_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);
    uint64_t last_events, last_promotions, last_demotions;
    sum_workload_counters(ctx, &last_events, &last_promotions, &last_demotions);
    uint64_t last_tsc = ctx->start_tsc;

    while (ctx->running) {
        /* Logging levels are fixed before threads start. Do not walk the PAC
         * table or compute rates when this coroutine's INFO output is disabled. */
        if (global_log_level < LOG_LEVEL_INFO) {
            mco_yield(co);
            continue;
        }
        uint64_t now = rdtsc();
        double elapsed_sec = (double)(now - last_tsc) / ctx->tsc_freq_hz;
        uint64_t events, promotions, demotions;
        sum_workload_counters(ctx, &events, &promotions, &demotions);

        uint64_t events_delta = events - last_events;
        uint64_t promotions_delta = promotions - last_promotions;
        uint64_t demotions_delta = demotions - last_demotions;

        log_info("stats_coroutine",
                 "Time passed %.2fs, Events: %lu/sec, Pages promoted: %lu/sec, "
                 "Pages demoted: %lu/sec, Hash entries: %u",
                 elapsed_sec, (uint64_t)(events_delta / elapsed_sec),
                 (uint64_t)(promotions_delta / elapsed_sec),
                 (uint64_t)(demotions_delta / elapsed_sec), total_hash_entries(ctx));

        log_info("stats_coroutine", "  PAC: pool_alloc_skip=%lu", ctx->stats.pool_alloc_skipped);
        log_workload_summary(ctx);
        log_pac_telemetry(ctx);

        last_events = events;
        last_promotions = promotions;
        last_demotions = demotions;
        last_tsc = now;
        mco_yield(co);
    }
}
