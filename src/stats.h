/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* stats.h: comprehensive stats output at exit. */

#ifndef PACT_PRINT_STATS_H
#define PACT_PRINT_STATS_H

#include "pact.h"

/* Print PACT statistics to stdout. Called at exit. */
void print_stats(pact_context_t *pact);

#endif /* PACT_PRINT_STATS_H */
