/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include "error.h"

int global_log_level = LOG_LEVEL_INFO;

static void noop_log(const char *function, const char *format, ...)
{
    (void)function;
    (void)format;
}

static void log_error_impl(const char *function, const char *format, ...);
static void log_warning_impl(const char *function, const char *format, ...);
static void log_info_impl(const char *function, const char *format, ...);
static void log_debug_impl(const char *function, const char *format, ...);
static void log_trace_impl(const char *function, const char *format, ...);

/* Static initializers must match global_log_level (LOG_LEVEL_INFO): debug
 * and trace stay silent unless --log-level raises the level. */
void (*log_error_fn)(const char *function, const char *format, ...) = log_error_impl;
void (*log_warning_fn)(const char *function, const char *format, ...) = log_warning_impl;
void (*log_info_fn)(const char *function, const char *format, ...) = log_info_impl;
void (*log_debug_fn)(const char *function, const char *format, ...) = noop_log;
void (*log_trace_fn)(const char *function, const char *format, ...) = noop_log;

/* Thread-safety contract ():
 *
 *   set_log_level() reassigns five function-pointer globals
 *   (log_error_fn / log_warning_fn / log_info_fn / log_debug_fn / log_trace_fn).
 *   These reassignments are NOT atomic and have no memory-barrier protection.
 *
 *   INVARIANT: set_log_level() MUST be called exactly once during single-threaded
 *   startup (before pthread_create or coroutine spawn), and MUST NOT be called
 *   again once worker threads are running.
 *
 *   All current callers (parse_command_line_args during main()'s init phase)
 *   satisfy this. If runtime log-level changes are ever needed, this function
 *   needs a pthread_mutex AND atomic stores; do not just sprinkle pthread_mutex —
 *   readers (every log call on the hot path) also need acquire-loads. */
void set_log_level(int level)
{
    global_log_level = level;

    log_error_fn = log_error_impl; /* always log errors */
    log_warning_fn = (level >= LOG_LEVEL_WARNING) ? log_warning_impl : noop_log;
    log_info_fn = (level >= LOG_LEVEL_INFO) ? log_info_impl : noop_log;
    log_debug_fn = (level >= LOG_LEVEL_DEBUG) ? log_debug_impl : noop_log;
    log_trace_fn = (level >= LOG_LEVEL_TRACE) ? log_trace_impl : noop_log;
}

static void log_error_impl(const char *function, const char *format, ...)
{
    fprintf(stderr, "[ERROR] %s: ", function);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, " (errno: %s)\n", strerror(errno));
}

static void log_warning_impl(const char *function, const char *format, ...)
{
    fprintf(stderr, "[WARNING] %s: ", function);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void log_info_impl(const char *function, const char *format, ...)
{
    fprintf(stdout, "[INFO] %s: ", function);
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void log_debug_impl(const char *function, const char *format, ...)
{
    fprintf(stdout, "[DEBUG] %s: ", function);
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void log_trace_impl(const char *function, const char *format, ...)
{
    fprintf(stdout, "[TRACE] %s: ", function);
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

void *safe_malloc(size_t size, const char *context)
{
    void *ptr = malloc(size);
    if (!ptr) {
        log_error(context, "Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *safe_calloc(size_t nmemb, size_t size, const char *context)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        log_error(context, "Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

int safe_close(int fd, const char *context)
{
    if (fd >= 0) {
        if (close(fd) < 0) {
            log_error(context, "Failed to close file descriptor");
            return -1;
        }
    }
    return 0;
}
