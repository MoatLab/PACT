/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* cooling.c: cooling factor α (Algorithm 1).
 *
 * PAC update has form pac[p] ← α · pac[p] + S_p with α ∈ [0, 1]. Default
 * α = 1.0 (pure accumulation, no cooling). When α < 1.0, cooling is
 * triggered every cooling_trigger_samples PEBS samples (default 200000):
 * the cooling coroutine walks every PAC entry and multiplies pac_value
 * by α (or zeros it when α = 0).
 */

#include <stdint.h>

#include "pact.h"
#include "cooling.h"

/* Apply α decay to one PAC entry. α = 1.0 → no-op; α = 0 → reset; otherwise
 * multiply. */
static inline void cool_one_entry(pac_metadata_t *meta, double alpha)
{
    if (alpha == 0.0) {
        meta->pac_value = 0;
    } else {
        meta->pac_value = (uint64_t)((double)meta->pac_value * alpha);
    }
}

/* Sweep one PAC table: apply cooling to every entry, yielding every 10K
 * processed to keep other coroutines responsive. */
static void cool_pac_table(pact_context_t *ctx, pac_table_t *table, double alpha, mco_coro *co)
{
    uint64_t processed = 0;
    khint_t k;
    kh_foreach(table, k)
    {
        cool_one_entry(kh_val(table, k), alpha);
        if (++processed % 10000 == 0) {
            mco_yield(co);
        }
    }
    ctx->stats.cooling_decays += processed;
}

/* The trigger counts the samples applied to each PAC table and cools that
 * table: PACT's one table counts every workload's samples. */
void cooling_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);

    while (ctx->running) {
        /* Default α = 1.0: no cooling. Skip the table walk entirely. */
        if (ctx->cooling_alpha < 1.0) {
            for (int d = 0; d < ctx->nr_domains && ctx->running; d++) {
                pac_domain_t *domain = &ctx->domains[d];
                uint64_t current = domain->samples;
                if (current - domain->cooling_last_trigger >= ctx->cooling_trigger_samples) {
                    cool_pac_table(ctx, domain->table, ctx->cooling_alpha, co);
                    domain->cooling_last_trigger = current;
                }
            }
        }
        mco_yield(co);
    }
}
