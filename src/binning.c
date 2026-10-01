/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* binning.c — adaptive binning (Freedman-Diaconis), Algorithm 3. */

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "pact.h"
#include "binning.h"
#include "tsc.h" /* rdtsc */

/* TH_SCALE threshold for bin-width doubling/halving. */
#ifndef PACT_BINNING_TH_SCALE
#define PACT_BINNING_TH_SCALE 0.7
#endif

/* Three-way selection skips equal scores together. PAC reservoirs often
 * contain repeated values; a two-way partition makes those inputs quadratic.
 * Iterate rather than recurse so adversarial inputs cannot grow the stack. */
static double quickselect(double *arr, size_t low, size_t high, size_t k)
{
    while (low < high) {
        double pivot = arr[low + (high - low) / 2];
        size_t less = low, scan = low, greater = high;

        while (scan <= greater) {
            double value = arr[scan];
            if (value < pivot) {
                arr[scan++] = arr[less];
                arr[less++] = value;
            } else if (value > pivot) {
                arr[scan] = arr[greater];
                arr[greater--] = value;
            } else {
                scan++;
            }
        }

        if (k < less) {
            high = less - 1;
        } else if (k > greater) {
            low = greater + 1;
        } else {
            return arr[k];
        }
    }
    return arr[low];
}

void calculate_quartiles(reservoir_t *r, double *q1, double *q3)
{
    if (r->count < 4) {
        *q1 = *q3 = 0.0;
        return;
    }

    double *work = malloc(r->count * sizeof(double));
    if (!work) {
        /* do not crash mid-bin-width-computation on OOM */
        *q1 = *q3 = 0.0;
        return;
    }
    memcpy(work, r->samples, r->count * sizeof(double));

    size_t q1_idx = r->count / 4;
    size_t q3_idx = (3 * r->count) / 4;

    *q1 = quickselect(work, 0, r->count - 1, q1_idx);
    /* For Q3, we can reuse the partially sorted array */
    *q3 = quickselect(work, q1_idx, r->count - 1, q3_idx);

    free(work);
}

void update_bin_width(pact_context_t *pact)
{
    pact_workload_t *wl = pact->workload;
    binning_state_t *bin = wl->binning;

    double q1, q3;
    calculate_quartiles(wl->reservoir, &q1, &q3);
    if (q3 <= q1) {
        return;
    }

    size_t n = kh_size(wl->pac_table);
    if (n == 0) {
        return;
    }

    double iqr = q3 - q1;
    double n_cbrt = cbrt((double)n);
    double new_width = 2.0 * iqr / n_cbrt;

    bin->q1 = q1;
    bin->q3 = q3;

    /* Pressure adjust (Algorithm 3): scale up if the table is much
     * larger than the promotion-candidate set (most pages cold → widen
     * bins to compress the long tail). N_c is the migration ring fill. */
    size_t n_c = wl->migration_ring ? ring_buffer_migration_entry_size(wl->migration_ring) : 0;
    if (n_c > 0) {
        if ((double)n / (double)n_c > PACT_BINNING_TH_SCALE) {
            new_width = 2 * new_width;
        } else {
            new_width = new_width / 2;
        }
    }
    bin->bin_width = new_width;
    bin->last_update = rdtsc();
}
