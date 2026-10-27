/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* admission.h: Tierce-mode admission, per PAC table (domain).
 *
 * Each domain keeps a page reservoir: a uniform sample of its pages. At the
 * end of each epoch, a domain that had PAC updates in the epoch refreshes
 * its bar, the 0.90 quantile of the current PAC values of the members.
 * Without LDLAT every member counts. With LDLAT only the members on the
 * slow tier count, with the kernel's node for the members that the runtime
 * holds on the fast tier. The domain then tests each slow-tier page updated
 * in the epoch once against the new bar and queues the admitted pages for
 * promotion. A page whose PAC equals the bar is admitted. A domain without
 * updates keeps its bar. A domain without a member that counts admits
 * nothing in that epoch.
 */

#ifndef PACT_ADMISSION_H
#define PACT_ADMISSION_H

#include <stddef.h>
#include <stdint.h>

#include "pact.h"

/* Pages in each domain's reservoir. */
#define TIERCE_RESERVOIR_PAGES 1000

/* Allocate a reservoir of `capacity` pages. Exits on allocation failure
 * (startup only). */
page_reservoir_t *page_reservoir_create(size_t capacity);
void page_reservoir_destroy(page_reservoir_t *r);

/* Offer a page that just entered the domain's table. With n pages inserted
 * so far, the page joins with probability capacity / n and replaces a
 * random member. */
void page_reservoir_add(page_reservoir_t *r, pac_metadata_t *meta);

/* Index of the 0.90 quantile among n sorted values: floor(0.90 * n),
 * capped at n - 1 (n >= 1). With 1 to 3 values this is the largest. */
size_t promo_bar_index(size_t n);

/* The bar with LDLAT: the 0.90 quantile of the current PAC values of the
 * members on the slow tier. Members on the fast tier stay in the reservoir
 * but do not count. For each member that the runtime holds on the fast
 * tier, the kernel's node decides (r->query_nodes): a member on node 1
 * counts. If the query fails, only the members the runtime holds on the
 * slow tier count.
 * Returns 0, or -1 when no member is on the slow tier. */
int page_reservoir_bar(page_reservoir_t *r, double *bar);

/* List a slow-tier page updated in this epoch, once per epoch. */
void epoch_list_page(pac_domain_t *domain, pac_metadata_t *meta, uint32_t epoch);

/* End the current epoch: refresh the bar of each domain with PAC updates
 * (over all members without LDLAT, with page_reservoir_bar() with LDLAT),
 * admit its listed pages into its migration ring, and advance the epoch
 * number. Returns the number of pages queued. */
size_t end_pac_epoch(pact_context_t *ctx);

#endif /* PACT_ADMISSION_H */
