/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* tsc.h: TSC conversion helpers.
 *
 * All TSC/time conversions MUST go through these inlines, which use
 * `ctx->tsc_freq_hz` (detected at runtime via `pact_detect_tsc_frequency()`
 * during pact_init). Never embed a hardcoded clock rate.
 */

#ifndef PACT_TSC_H
#define PACT_TSC_H

#include <stdint.h>

#include "pact.h" /* pact_context_t (for tsc_freq_hz field) */

/* Read the timestamp counter. Single source of truth: all *.c files should
 * include this header and use this inline rather than redefining their own. */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t sec_to_tsc(pact_context_t *ctx, uint64_t sec)
{
    return sec * ctx->tsc_freq_hz;
}
static inline uint64_t ms_to_tsc(pact_context_t *ctx, uint64_t ms)
{
    return ms * ctx->tsc_freq_hz / 1000;
}
static inline uint64_t tsc_to_ms(pact_context_t *ctx, uint64_t tsc)
{
    return tsc * 1000 / ctx->tsc_freq_hz;
}
static inline uint64_t tsc_to_us(pact_context_t *ctx, uint64_t tsc)
{
    return tsc * 1000000 / ctx->tsc_freq_hz;
}

#endif /* PACT_TSC_H */
