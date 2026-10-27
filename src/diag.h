/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* diag.h: diagnostics of a `make DIAG=1` build (PACT_DIAG).
 *
 * --dump-pac-table FILE writes a snapshot of every PAC table every
 * --dump-pac-interval seconds (default 30): one header block per table
 * with its admission bar (PACT: the lowest PAC of the top bin; Tierce: the
 * table's 0.90-quantile bar), then one row per tracked page with a PAC
 * above 0. A table larger than PAC_DUMP_MAX_ROWS rows is thinned by a
 * deterministic hash of the key.
 *
 * --no-promote-wl IDX (repeatable) keeps workload IDX out of promotion:
 * Tierce does not queue its admitted pages, PACT does not queue its pages
 * in the global queue. Sampling, PAC updates and the bars are unchanged.
 */

#ifndef PACT_DIAG_H
#define PACT_DIAG_H

#ifdef PACT_DIAG

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct pact_context;

/* Default --dump-pac-interval, seconds. */
#define PAC_DUMP_DEFAULT_INTERVAL_S 30U
/* Row target per table per snapshot. It sets the hash thinning rate. */
#define PAC_DUMP_MAX_ROWS 50000

typedef struct pac_dump {
    FILE *file;
    char *buf; /* large stdio buffer: few write() calls per snapshot */
    uint32_t interval_s;
    uint64_t next_tsc; /* event-loop deadline of the next snapshot; 0 = off */
    uint64_t snapshots;
} pac_dump_t;

/* Open the dump and write its header. Returns 0, or -1 when the file
 * cannot be opened (the run continues without the dump). */
int pac_dump_open(struct pact_context *ctx, const char *path, uint32_t interval_s);

/* Write one snapshot of every PAC table. */
void pac_dump_snapshot(struct pact_context *ctx);

/* Close the dump. Safe to call when it is off. */
void pac_dump_close(struct pact_context *ctx);

/* The AND mask that thins n rows by a power of two toward
 * PAC_DUMP_MAX_ROWS (a page is written when hash & mask == 0, mask 0 writes
 * every row). */
uint32_t pac_dump_subsample_mask(size_t n);

#endif /* PACT_DIAG */

#endif /* PACT_DIAG_H */
