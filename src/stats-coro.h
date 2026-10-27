/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* stats-coro.h: periodic stats coroutine. */

#ifndef PACT_STATS_CORO_H
#define PACT_STATS_CORO_H

#include "minicoro.h"

/* Coroutine body: yields every stats_interval_ms to log stats. */
void stats_coroutine(mco_coro *co);

#endif /* PACT_STATS_CORO_H */
