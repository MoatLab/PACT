/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#ifndef PACT_ERROR_H
#define PACT_ERROR_H

#include <stddef.h> /* size_t (safe_malloc/safe_calloc below) */

/* Error-handling convention.
 *
 * RETURN-CODE CONVENTION
 *
 *   int-returning functions:
 *     0     success
 *     < 0   failure (-errno or -1 for generic)
 *     > 0   "informational success" (e.g., pact_parse_command_line_args returns 1
 *           when --help was requested, to signal early exit without error)
 *
 *   pointer-returning functions:
 *     non-NULL  success
 *     NULL      failure; errno may or may not be set (most callers in PACT
 *               do NOT consult errno after a NULL: only relevant for
 *               malloc/calloc/mmap-wrapping functions, e.g., pool_alloc)
 *
 *   void-returning functions: cannot fail, OR failure is logged and
 *     swallowed (do not introduce new void-returning functions that can
 *     silently fail at a critical site).
 *
 * ALLOCATION CHECKS
 *
 *   Internal allocations on the hot path: MUST check NULL. Failure modes:
 *     - safe_malloc / safe_calloc: log + exit (fatal: use for startup only)
 *     - hot-path malloc: log_warning + degrade (return NULL up the stack,
 *       or skip-and-continue with a stats counter incremented)
 *
 * CRASH MARKER
 *
 *   --crash-marker PATH causes PACT to write:
 *     - "crash sig=NN\n" on SIGSEGV/ABRT/BUS/ILL/FPE (before re-raising)
 *     - "ok signal=N\n" on clean exit (signal = received SIGINT/TERM/HUP, or 0)
 *   Wrapper scripts can read this file to distinguish crash vs clean exit
 *   without parsing stderr.
 */

#include <errno.h>

#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARNING 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3
#define LOG_LEVEL_TRACE 4

extern int global_log_level;

extern void (*log_error_fn)(const char *function, const char *format, ...);
extern void (*log_warning_fn)(const char *function, const char *format, ...);
extern void (*log_info_fn)(const char *function, const char *format, ...);
extern void (*log_debug_fn)(const char *function, const char *format, ...);
extern void (*log_trace_fn)(const char *function, const char *format, ...);

void set_log_level(int level);

#define log_error log_error_fn
#define log_warning log_warning_fn
#define log_info log_info_fn
#define log_debug log_debug_fn
#define log_trace log_trace_fn

void *safe_malloc(size_t size, const char *context);
void *safe_calloc(size_t nmemb, size_t size, const char *context);
int safe_close(int fd, const char *context);

#endif
