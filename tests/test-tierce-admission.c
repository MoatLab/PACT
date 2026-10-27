/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* Tierce admission: the 0.90-quantile bar of the page reservoir, the epoch
 * page list, and the end-of-epoch admission per domain. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../src/admission.h"
#include "../src/error.h"

static pac_metadata_t *page(uint64_t pac, uint8_t tier, uint8_t workload)
{
    pac_metadata_t *meta = calloc(1, sizeof(*meta));
    assert(meta);
    meta->pac_value = pac;
    meta->tier = tier;
    meta->workload = workload;
    return meta;
}

static void test_bar_index(void)
{
    assert(promo_bar_index(1) == 0);
    assert(promo_bar_index(2) == 1);
    assert(promo_bar_index(3) == 2);
    assert(promo_bar_index(4) == 3);
    assert(promo_bar_index(10) == 9);
    assert(promo_bar_index(11) == 9);
    assert(promo_bar_index(20) == 18);
    assert(promo_bar_index(1000) == 900);
}

static void test_reservoir(void)
{
    page_reservoir_t *r = page_reservoir_create(4);
    double bar;
    assert(page_reservoir_bar(r, &bar) < 0);

    pac_metadata_t *one = page(70, 1, 0);
    page_reservoir_add(r, one);
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 70.0); /* one page: its PAC */

    pac_metadata_t *more[3] = {page(10, 1, 0), page(40, 1, 0), page(20, 1, 0)};
    for (int i = 0; i < 3; i++) {
        page_reservoir_add(r, more[i]);
    }
    assert(r->count == 4 && r->total_seen == 4);
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 70.0); /* index 3 of 4: the largest */

    /* The bar reads the members' current PAC values. */
    one->pac_value = 5;
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 40.0);

    /* A full reservoir keeps its size; later pages may replace members. */
    pac_metadata_t *extra[100];
    for (int i = 0; i < 100; i++) {
        extra[i] = page(1000, 1, 0);
        page_reservoir_add(r, extra[i]);
    }
    assert(r->count == 4 && r->total_seen == 104);
    page_reservoir_destroy(r);
    free(one);
    for (int i = 0; i < 3; i++) {
        free(more[i]);
    }
    for (int i = 0; i < 100; i++) {
        free(extra[i]);
    }
}

/* Only the members on the slow tier set the bar. Fast-tier members stay in
 * the reservoir and count again when they return to the slow tier. */
static void test_reservoir_slow_tier(void)
{
    page_reservoir_t *r = page_reservoir_create(8);
    double bar;
    pac_metadata_t *slow[3] = {page(10, 1, 0), page(20, 1, 0), page(30, 1, 0)};
    pac_metadata_t *fast[2] = {page(100, 0, 0), page(200, 0, 0)};
    for (int i = 0; i < 2; i++) {
        page_reservoir_add(r, fast[i]);
    }
    assert(page_reservoir_bar(r, &bar) < 0); /* no slow-tier member */
    for (int i = 0; i < 3; i++) {
        page_reservoir_add(r, slow[i]);
    }
    assert(r->count == 5);
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 30.0); /* index 2 of 3 slow */

    fast[0]->tier = 1;                                        /* demoted: counts again */
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 100.0); /* index 3 of 4 slow */
    page_reservoir_destroy(r);
    for (int i = 0; i < 3; i++) {
        free(slow[i]);
    }
    for (int i = 0; i < 2; i++) {
        free(fast[i]);
    }
}

/* Stand-in for the kernel's node query: the node of each known address, a
 * missing page for any other address, or a failed query. */
static uint64_t node_addr[2];
static int node_of[2];
static bool node_query_fails;
static int node_queries;

static long stub_page_nodes(int pid, unsigned long count, void **pages, int *status)
{
    (void)pid;
    node_queries++;
    if (node_query_fails) {
        return -1;
    }
    for (unsigned long i = 0; i < count; i++) {
        status[i] = -ENOENT;
        for (int k = 0; k < 2; k++) {
            if ((uint64_t)(uintptr_t)pages[i] == node_addr[k]) {
                status[i] = node_of[k];
            }
        }
    }
    return 0;
}

/* The kernel demotes pages without telling the runtime. A member that the
 * runtime holds on the fast tier counts when the kernel puts it on node 1,
 * and the runtime's tier of the page does not change. */
static void test_reservoir_kernel_node(void)
{
    page_reservoir_t *r = page_reservoir_create(8);
    r->query_nodes = stub_page_nodes;
    double bar;
    pac_metadata_t *slow[3] = {page(10, 1, 0), page(20, 1, 0), page(30, 1, 0)};
    pac_metadata_t *fast[2] = {page(100, 0, 0), page(200, 0, 0)};
    for (int k = 0; k < 2; k++) {
        fast[k]->page_addr = 0x1000 * (uint64_t)(k + 1);
        node_addr[k] = fast[k]->page_addr;
        node_of[k] = 0;
        page_reservoir_add(r, fast[k]);
    }
    for (int i = 0; i < 3; i++) {
        page_reservoir_add(r, slow[i]);
    }
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 30.0); /* both on node 0 */

    node_of[1] = 1;                                           /* demoted by the kernel */
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 200.0); /* index 3 of 4 */
    assert(fast[1]->tier == 0);

    node_query_fails = true; /* only the runtime's slow-tier members count */
    assert(r->query_failures == 0);
    assert(page_reservoir_bar(r, &bar) == 0 && bar == 30.0);
    assert(page_reservoir_bar(r, &bar) == 0 && r->query_failures == 2); /* each one counts */
    node_query_fails = false;
    assert(page_reservoir_bar(r, &bar) == 0 && r->query_failures == 2);

    for (int i = 0; i < 3; i++) {
        slow[i]->tier = 0; /* no member left on the slow tier by either view */
    }
    node_of[1] = 0;
    assert(page_reservoir_bar(r, &bar) < 0);
    page_reservoir_destroy(r);
    for (int i = 0; i < 3; i++) {
        free(slow[i]);
    }
    for (int i = 0; i < 2; i++) {
        free(fast[i]);
    }
}

/* End one epoch of a domain with one listed page and return the pages
 * queued. An admitted page leaves the ring and is no longer migrating. */
static size_t end_epoch_listing(pact_context_t *ctx, pac_metadata_t *listed)
{
    pac_domain_t *domain = &ctx->domains[0];
    epoch_list_page(domain, listed, ctx->pac_epoch);
    domain->epoch_updates = 1;
    size_t queued = end_pac_epoch(ctx);
    migration_entry_t entry;
    while (ring_buffer_migration_entry_pop(domain->migration_ring, &entry)) {
        entry.meta->migrating = false;
    }
    return queued;
}

/* Without LDLAT every member sets the bar: a fast-tier member counts, and
 * the kernel is not asked. With LDLAT only the members on the slow tier
 * count: a member that the runtime holds on the fast tier counts only when
 * the kernel puts it on node 1. */
static void test_bar_mode(void)
{
    pact_workload_t workloads[1] = {{.index = 0, .domain = 0}};
    pac_domain_t domain;
    memset(&domain, 0, sizeof(domain));
    domain.pages = page_reservoir_create(8);
    domain.pages->query_nodes = stub_page_nodes;
    domain.migration_ring = ring_buffer_migration_entry_create(4);
    assert(domain.migration_ring);
    pact_context_t ctx = {.mode = PACT_MODE_TIERCE,
                          .workloads = workloads,
                          .nr_workloads = 1,
                          .domains = &domain,
                          .nr_domains = 1,
                          .pac_epoch = 1};
    pac_metadata_t *slow[3] = {page(10, 1, 0), page(20, 1, 0), page(30, 1, 0)};
    pac_metadata_t *fast = page(200, 0, 0);
    fast->page_addr = 0x1000;
    node_addr[0] = fast->page_addr;
    node_of[0] = 0;
    node_addr[1] = 0;
    node_query_fails = false;
    page_reservoir_add(domain.pages, fast);
    for (int i = 0; i < 3; i++) {
        page_reservoir_add(domain.pages, slow[i]);
    }

    node_queries = 0;
    ctx.use_ldlat = false;
    assert(end_epoch_listing(&ctx, slow[2]) == 0);
    assert(domain.bar == 200.0 && node_queries == 0); /* index 3 of 4 members */

    ctx.use_ldlat = true;
    assert(end_epoch_listing(&ctx, slow[2]) == 1);
    assert(domain.bar == 30.0 && node_queries == 1); /* fast member on node 0 */

    node_of[0] = 1; /* demoted by the kernel */
    assert(end_epoch_listing(&ctx, slow[2]) == 0);
    assert(domain.bar == 200.0 && node_queries == 2); /* index 3 of 4 slow */
    assert(fast->tier == 0);
    assert(domain.no_bar_epochs == 0);

    /* No member on the slow tier by either view: the epoch has no bar, it
     * admits nothing, and the stats count it. The old bar stays. */
    node_of[0] = 0;
    for (int i = 0; i < 3; i++) {
        slow[i]->tier = 0;
    }
    assert(end_epoch_listing(&ctx, slow[2]) == 0);
    assert(domain.no_bar_epochs == 1 && domain.bar == 200.0);
    ctx.use_ldlat = false; /* every member counts again */
    assert(end_epoch_listing(&ctx, slow[2]) == 0 && domain.no_bar_epochs == 1);

    page_reservoir_destroy(domain.pages);
    free(domain.epoch_pages);
    ring_buffer_migration_entry_destroy(domain.migration_ring);
    for (int i = 0; i < 3; i++) {
        free(slow[i]);
    }
    free(fast);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    test_bar_index();
    test_reservoir_slow_tier();
    test_reservoir_kernel_node();
    test_reservoir();
    test_bar_mode();

    /* Two domains: domain 0 has updates this epoch, domain 1 has none. */
    pact_workload_t workloads[2] = {{.index = 0, .domain = 0}, {.index = 1, .domain = 1}};
    pac_domain_t domains[2];
    memset(domains, 0, sizeof(domains));
    for (int d = 0; d < 2; d++) {
        domains[d].pages = page_reservoir_create(TIERCE_RESERVOIR_PAGES);
        domains[d].migration_ring = ring_buffer_migration_entry_create(4);
        assert(domains[d].migration_ring);
    }
    pact_context_t ctx = {.mode = PACT_MODE_TIERCE,
                          .workloads = workloads,
                          .nr_workloads = 2,
                          .domains = domains,
                          .nr_domains = 2,
                          .pac_epoch = 1};

    /* Ten pages with PAC 10..100: the bar is the value at index 9 (100). */
    pac_metadata_t *pages[10];
    for (int i = 0; i < 10; i++) {
        pages[i] = page((uint64_t)(i + 1) * 10, 1, 0);
        page_reservoir_add(domains[0].pages, pages[i]);
    }
    pages[8]->pac_value = 100; /* ties the bar: admitted */
    for (int i = 0; i < 10; i++) {
        epoch_list_page(&domains[0], pages[i], ctx.pac_epoch);
        epoch_list_page(&domains[0], pages[i], ctx.pac_epoch); /* listed once */
    }
    assert(domains[0].nr_epoch_pages == 10);
    domains[0].epoch_updates = 20;
    pages[9]->migrating = 1; /* already queued: skipped */
    domains[1].bar = 55.0;

    assert(end_pac_epoch(&ctx) == 1);
    assert(domains[0].bar == 100.0);
    assert(ring_buffer_migration_entry_size(domains[0].migration_ring) == 1);
    migration_entry_t entry;
    assert(ring_buffer_migration_entry_pop(domains[0].migration_ring, &entry));
    assert(entry.meta == pages[8] && entry.target_node == 0 && pages[8]->migrating);
    assert(workloads[0].stats.promotion_attempts == 1);
    assert(domains[0].nr_epoch_pages == 0 && domains[0].epoch_updates == 0);
    assert(domains[1].bar == 55.0); /* no updates: the bar stays */
    assert(ctx.pac_epoch == 2);

    /* A fast-tier page is not admitted; a full ring drops the page. */
    pages[8]->migrating = 0;
    pages[9]->migrating = 0;
    for (int i = 0; i < 10; i++) {
        pages[i]->pac_value = 500;
        epoch_list_page(&domains[0], pages[i], ctx.pac_epoch);
    }
    pages[0]->tier = 0;
    domains[0].epoch_updates = 10;
    /* Capacity 3: three pages fit, the other six are ring-full drops. */
    assert(end_pac_epoch(&ctx) == 3);
    assert(domains[0].ring_full_drops == 6);
    assert(!pages[9]->migrating);

    /* The epoch number skips 0, which marks a page never listed. */
    ctx.pac_epoch = UINT32_MAX;
    assert(end_pac_epoch(&ctx) == 0);
    assert(ctx.pac_epoch == 1);

    for (int i = 0; i < 10; i++) {
        free(pages[i]);
    }
    for (int d = 0; d < 2; d++) {
        page_reservoir_destroy(domains[d].pages);
        free(domains[d].epoch_pages);
        ring_buffer_migration_entry_destroy(domains[d].migration_ring);
    }
    return 0;
}
