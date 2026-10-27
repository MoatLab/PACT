/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* cooling.h: cooling/decay coroutine for PAC metadata.
 *
 * Implements the α-decay step:
 * pac[p] = α · pac[p] every cooling-trigger-samples PEBS samples.
 * Default α=1.0 = no cooling. The coroutine then skips the table walk.
 */

#ifndef PACT_COOLING_H
#define PACT_COOLING_H

#include "pact.h"
#include "minicoro.h"

void cooling_coroutine(mco_coro *co);

#endif /* PACT_COOLING_H */
