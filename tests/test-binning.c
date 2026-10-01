/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "binning.h"

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Compare exact order statistics against an independent full-sort oracle,
 * including the production reservoir size and duplicate-heavy PAC scores. */
int main(int argc, char **argv)
{
    double samples[4096], sorted[4096], original[4096];
    unsigned int seed = 17;
    for (size_t n = 0; n <= 4096; n = n < 128 ? n + 1 : n * 2) {
        for (int pattern = 0; pattern < 5; pattern++) {
            for (size_t i = 0; i < n; i++) {
                seed = seed * 1664525u + 1013904223u;
                samples[i] = pattern == 0   ? 42.0
                             : pattern == 1 ? (double)i
                             : pattern == 2 ? (double)(n - i)
                             : pattern == 3 ? (double)(seed % 7)
                                            : (double)seed;
            }
            memcpy(original, samples, n * sizeof(double));
            memcpy(sorted, samples, n * sizeof(double));
            qsort(sorted, n, sizeof(double), compare);
            reservoir_t r = {.samples = samples, .count = n, .capacity = n};
            double q1, q3;
            calculate_quartiles(&r, &q1, &q3);
            assert(q1 == (n < 4 ? 0 : sorted[n / 4]));
            assert(q3 == (n < 4 ? 0 : sorted[3 * n / 4]));
            assert(memcmp(original, samples, n * sizeof(double)) == 0);
        }
    }
    puts("quartiles: sorted-oracle and input-preservation checks passed");

    /* Optional microbenchmark, never a timing-based correctness assertion. */
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        reservoir_t r = {.samples = samples, .count = 100, .capacity = 100};
        for (int pattern = 0; pattern < 3; pattern++) {
            for (size_t i = 0; i < r.count; i++) {
                samples[i] = pattern == 0 ? 42 : pattern == 1 ? (double)i : (double)(i % 7);
            }
            clock_t start = clock();
            double q1, q3;
            for (int i = 0; i < 100000; i++) {
                calculate_quartiles(&r, &q1, &q3);
            }
            printf("pattern=%d n=100 iterations=100000 cpu_seconds=%.6f\n", pattern,
                   (double)(clock() - start) / CLOCKS_PER_SEC);
        }
    }
    return 0;
}
