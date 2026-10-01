/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* stats-coro.c — periodic stats coroutine.
 *
 *
 * Yields control every `stats_interval_ms` to update derived metrics +
 * call print_stats. Uses minicoro coroutines.
 */

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
    return kh_size(ctx->workload->pac_table);
}

static void log_workload_summary(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    log_info("stats_coroutine", "  WL (%s): hash=%u, Q1=%.1f, Q3=%.1f, bw=%.1f, pebs_samples=%lu",
             wl->name, kh_size(wl->pac_table), wl->binning->q1, wl->binning->q3,
             wl->binning->bin_width, wl->total_pebs_samples);
}

/* PAC histogram for the workload: min/avg/max + per-tier averages + count
 * over threshold. Samples up to ~10K entries from large tables. */
static void log_one_workload_pac_dist(pact_workload_t *wl)
{
    pac_table_t *table = wl->pac_table;
    uint32_t table_size = kh_size(table);
    if (table_size == 0) {
        return;
    }

    double threshold = wl->binning->bin_width * (wl->binning->bin_count - 1);
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
        /* Match the runtime's bin-index decision, including fractional widths.
         * Keep the displayed threshold floating-point: accepted widths may
         * produce a threshold beyond uint64_t without admitting any page. */
        if (!(wl->binning->bin_width > 0) ||
            (double)pv / wl->binning->bin_width >= (double)(wl->binning->bin_count - 1)) {
            n_above_threshold++;
        }
    }

    if (n_total == 0) {
        return;
    }
    log_info("stats_coroutine",
             "  WL PAC_DIST: n=%u min=%lu avg=%lu max=%lu threshold=%.17g above_thresh=%u "
             "fast(n=%u avg=%lu) slow(n=%u avg=%lu)",
             n_total, pac_min, (uint64_t)(pac_sum / n_total), pac_max, threshold, n_above_threshold,
             n_fast, n_fast > 0 ? (uint64_t)(pac_fast_sum / n_fast) : 0, n_slow,
             n_slow > 0 ? (uint64_t)(pac_slow_sum / n_slow) : 0);
}

static void log_pac_telemetry(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    log_info("stats_coroutine",
             "  WL PAC_INPUTS: llc_fast=%lu llc_slow=%lu mlp_fast=%.2f mlp_slow=%.2f "
             "k_dram=%lu k_cxl=%lu pebs_period=%u",
             wl->stats.llc_misses_fast, wl->stats.llc_misses_slow, wl->workload_mlp_fast,
             wl->workload_mlp_slow, ctx->k_constant_dram, ctx->k_constant_cxl,
             ctx->pebs_sampling_period);
    log_one_workload_pac_dist(wl);
}

void stats_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);
    pact_stats_t last_stats = ctx->workload->stats;
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
        pact_stats_t *st = &ctx->workload->stats;

        uint64_t events_delta = st->pebs_events_processed - last_stats.pebs_events_processed;
        uint64_t promotions_delta = st->promotion_successes - last_stats.promotion_successes;
        uint64_t demotions_delta = st->demotion_successes - last_stats.demotion_successes;

        log_info("stats_coroutine",
                 "Time passed %.2fs, Events: %lu/sec, Pages promoted: %lu/sec, "
                 "Pages demoted: %lu/sec, Hash entries: %u",
                 elapsed_sec, (uint64_t)(events_delta / elapsed_sec),
                 (uint64_t)(promotions_delta / elapsed_sec),
                 (uint64_t)(demotions_delta / elapsed_sec), total_hash_entries(ctx));

        log_info("stats_coroutine", "  PAC: pool_alloc_skip=%lu", st->pool_alloc_skipped);
        log_workload_summary(ctx);
        log_pac_telemetry(ctx);

        last_stats = *st;
        last_tsc = now;
        mco_yield(co);
    }
}
