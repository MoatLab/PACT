/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* admission.c: Tierce-mode admission (page reservoir, 0.90 bar, epochs). */

#include <errno.h>
#include <limits.h>
#include <numa.h>
#include <stdlib.h>
#include <string.h>

#include "admission.h"
#include "binning.h" /* pact_select_kth */
#include "error.h"
#include "utils.h" /* fast_prng_*, atomic_inc_relaxed */

/* First capacity of a domain's epoch page list; it doubles as needed. */
#define EPOCH_PAGES_INITIAL 4096

/* The node of each page, from move_pages(2) without a target node, so no
 * page moves. */
static long kernel_page_nodes(int pid, unsigned long count, void **pages, int *status)
{
    return numa_move_pages(pid, count, pages, NULL, status, 0);
}

page_reservoir_t *page_reservoir_create(size_t capacity)
{
    page_reservoir_t *r = safe_calloc(1, sizeof(*r), "page_reservoir_create");
    r->members = safe_calloc(capacity, sizeof(*r->members), "page_reservoir_create");
    r->values = safe_calloc(capacity, sizeof(*r->values), "page_reservoir_create");
    r->query_pages = safe_calloc(capacity, sizeof(*r->query_pages), "page_reservoir_create");
    r->query_status = safe_calloc(capacity, sizeof(*r->query_status), "page_reservoir_create");
    r->query_members = safe_calloc(capacity, sizeof(*r->query_members), "page_reservoir_create");
    r->query_nodes = kernel_page_nodes;
    r->capacity = capacity;
    fast_prng_init(&r->prng);
    return r;
}

void page_reservoir_destroy(page_reservoir_t *r)
{
    if (!r) {
        return;
    }
    free(r->members);
    free(r->values);
    free(r->query_pages);
    free(r->query_status);
    free(r->query_members);
    free(r);
}

void page_reservoir_add(page_reservoir_t *r, pac_metadata_t *meta)
{
    if (r->count < r->capacity) {
        r->members[r->count++] = meta;
    } else {
        size_t idx = fast_prng_next(&r->prng) % (r->total_seen + 1);
        if (idx < r->capacity) {
            r->members[idx] = meta;
        }
    }
    r->total_seen++;
}

size_t promo_bar_index(size_t n)
{
    size_t k = (n * 9) / 10; /* floor(0.90 * n) without rounding error */
    return k < n ? k : n - 1;
}

/* The kernel demotes pages without telling the runtime, so a member that
 * the runtime holds on the fast tier may be on the slow tier now. The bar
 * asks the kernel for the node of those members. A domain's pages belong
 * to one process, so members of another PID are not asked. */
int page_reservoir_bar(page_reservoir_t *r, double *bar)
{
    size_t n = 0, asked = 0;
    pid_t pid = 0;
    for (size_t i = 0; i < r->count; i++) {
        pac_metadata_t *meta = r->members[i];
        if (meta->tier == 1) {
            r->values[n++] = (double)meta->pac_value;
        } else if (asked == 0 || meta->pid == pid) {
            pid = meta->pid;
            r->query_pages[asked] = (void *)meta->page_addr;
            r->query_status[asked] = INT_MIN;
            r->query_members[asked++] = i;
        }
    }
    if (asked > 0 && r->query_nodes(pid, asked, r->query_pages, r->query_status) >= 0) {
        for (size_t j = 0; j < asked; j++) {
            if (r->query_status[j] == 1) {
                r->values[n++] = (double)r->members[r->query_members[j]]->pac_value;
            }
        }
    } else if (asked > 0) {
        /* The bar then counts only the members held on the slow tier. The
         * stats count every failure and the log shows the first one. */
        if (r->query_failures++ == 0) {
            log_warning("page_reservoir_bar",
                        "The node query of PID %d failed (%s); the bar counts only the members "
                        "held on the slow tier while the query fails",
                        (int)pid, strerror(errno));
        }
    }
    if (n == 0) {
        return -1;
    }
    *bar = pact_select_kth(r->values, n, promo_bar_index(n));
    return 0;
}

/* The 0.90 quantile of the current PAC values of all members, fast-tier
 * members included. Returns 0, or -1 when the reservoir is empty. */
static int all_member_bar(page_reservoir_t *r, double *bar)
{
    if (r->count == 0) {
        return -1;
    }
    for (size_t i = 0; i < r->count; i++) {
        r->values[i] = (double)r->members[i]->pac_value;
    }
    *bar = pact_select_kth(r->values, r->count, promo_bar_index(r->count));
    return 0;
}

void epoch_list_page(pac_domain_t *domain, pac_metadata_t *meta, uint32_t epoch)
{
    if (meta->epoch_stamp == epoch) {
        return;
    }
    if (domain->nr_epoch_pages == domain->epoch_pages_cap) {
        size_t cap = domain->epoch_pages_cap ? domain->epoch_pages_cap * 2 : EPOCH_PAGES_INITIAL;
        pac_metadata_t **grown = realloc(domain->epoch_pages, cap * sizeof(*grown));
        if (!grown) {
            if (domain->list_full_drops++ == 0) {
                log_error("epoch_list_page", "Cannot grow the epoch page list to %zu pages", cap);
            }
            return;
        }
        domain->epoch_pages = grown;
        domain->epoch_pages_cap = cap;
    }
    meta->epoch_stamp = epoch;
    domain->epoch_pages[domain->nr_epoch_pages++] = meta;
}

/* Test one listed page against the domain's bar and queue it when it is
 * admitted. A page that left the slow tier, or is already queued or
 * migrating, is skipped. Returns true when the page was queued. */
static bool admit_listed_page(pact_context_t *ctx, pac_domain_t *domain, pac_metadata_t *meta)
{
    if (meta->tier != 1 || meta->migrating || (double)meta->pac_value < domain->bar) {
        return false;
    }
#ifdef PACT_DIAG
    if (ctx->no_promote_mask & (1u << meta->workload)) {
        return false; /* --no-promote-wl */
    }
#endif
    meta->migrating = true;
    migration_entry_t entry = {.meta = meta, .target_node = 0};
    if (!ring_buffer_migration_entry_push(domain->migration_ring, entry)) {
        meta->migrating = false;
        domain->ring_full_drops++;
        return false;
    }
    atomic_inc_relaxed(&ctx->workloads[meta->workload].stats.promotion_attempts);
    return true;
}

size_t end_pac_epoch(pact_context_t *ctx)
{
    size_t queued = 0;
    for (int d = 0; d < ctx->nr_domains; d++) {
        pac_domain_t *domain = &ctx->domains[d];
        if (domain->epoch_updates == 0) {
            continue;
        }
        /* The bar needs members whose sample counts spread. Without LDLAT,
         * slow-tier pages carry few samples, so the bar uses all members,
         * the promoted pages included. With LDLAT, slow-tier pages carry many
         * samples, and promoted pages keep the high counts of their samples from
         * before the promotion. Those counts would hold the bar too high, so the
         * bar uses only the slow-tier members. */
        int got = ctx->use_ldlat ? page_reservoir_bar(domain->pages, &domain->bar)
                                 : all_member_bar(domain->pages, &domain->bar);
        if (got == 0) {
            for (size_t i = 0; i < domain->nr_epoch_pages; i++) {
                queued += admit_listed_page(ctx, domain, domain->epoch_pages[i]);
            }
        } else {
            domain->no_bar_epochs++;
        }
        domain->nr_epoch_pages = 0;
        domain->epoch_updates = 0;
    }
    /* 0 marks a page that was never listed. */
    if (++ctx->pac_epoch == 0) {
        ctx->pac_epoch = 1;
    }
    return queued;
}
