/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* binning.h: adaptive binning helpers (Algorithm 3). */

#ifndef PACT_BINNING_H
#define PACT_BINNING_H

#include "pact.h"

/* The k-th smallest of values[0..n-1] (0-based, n >= 1, k < n). Reorders
 * values in place. */
double pact_select_kth(double *values, size_t n, size_t k);

/* Compute quartiles Q1 + Q3 of reservoir samples. Allocates a working
 * copy. Returns 0.0/0.0 on OOM or count<4. */
void calculate_quartiles(reservoir_t *r, double *q1, double *q3);

/* Recompute bin_width of every binned PAC table via Freedman-Diaconis rule.
 * Updates bin->q1, bin->q3, bin->bin_width, bin->last_update. */
void update_bin_width(pact_context_t *pact);

#endif /* PACT_BINNING_H */
