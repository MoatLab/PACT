/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* sighandler.c: signal handling + crash marker.
 *
 *
 * Async-signal-safe paths use only write(2) / open(2) / close(2) / _exit(2).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sighandler.h"

/* File-scope state. set_crash_marker_path / install_handlers must be called
 * from a single-threaded startup phase (consistent with set_log_level in
 * error.c). */
static char g_crash_marker_path[1024] = "";
static volatile sig_atomic_t *volatile g_running_flag = NULL;
static volatile sig_atomic_t g_signal_received = 0;
static volatile sig_atomic_t g_shutdown_in_progress = 0;

void pact_signal_set_crash_marker_path(const char *path)
{
    if (!path || path[0] == '\0') {
        g_crash_marker_path[0] = '\0';
        return;
    }
    strncpy(g_crash_marker_path, path, sizeof(g_crash_marker_path) - 1);
    g_crash_marker_path[sizeof(g_crash_marker_path) - 1] = '\0';
}

void pact_signal_clear_running_flag(void)
{
    g_running_flag = NULL;
}

int pact_signal_received(void)
{
    return g_signal_received;
}

/* Async-signal-safe write of a byte range to fd, ignoring the result (we are
 * in a signal handler with nowhere to report a short/failed write). */
static void safe_write(int fd, const char *buf, size_t len)
{
    ssize_t r = write(fd, buf, len);
    (void)r;
}

/* Async-signal-safe crash marker writer.
 * Format: "crash sig=NN\n" */
static void write_crash_marker_sigsafe(int sig)
{
    if (g_crash_marker_path[0] == '\0') {
        return;
    }
    int fd = open(g_crash_marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    /* Manual itoa to stay signal-safe */
    char buf[64];
    int n = 0;
    const char *prefix = "crash sig=";
    while (prefix[n]) {
        buf[n] = prefix[n];
        n++;
    }
    if (sig >= 100) {
        buf[n++] = '0' + sig / 100;
        sig %= 100;
    }
    if (sig >= 10 || n > 10) {
        buf[n++] = '0' + sig / 10;
        sig %= 10;
    }
    buf[n++] = '0' + sig;
    buf[n++] = '\n';
    safe_write(fd, buf, (size_t)n);
    close(fd);
}

/* Fatal signal handler: write marker, restore default handler, re-raise
 * to produce a core dump. */
static void fatal_signal_handler(int sig)
{
    write_crash_marker_sigsafe(sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Graceful-shutdown signal handler (SIGINT/SIGTERM/SIGHUP). It saves and
 * restores errno, so the code that it interrupts sees its own errno. */
static void graceful_signal_handler(int sig)
{
    int saved_errno = errno;
    g_signal_received = sig;

    if (g_shutdown_in_progress) {
        /* Second signal during shutdown: force exit */
        if (sig == SIGINT || sig == SIGTERM) {
            const char msg[] = "\nForced shutdown requested. Exiting immediately.\n";
            safe_write(STDOUT_FILENO, msg, sizeof(msg) - 1);
            _exit(1);
        }
        errno = saved_errno;
        return;
    }
    g_shutdown_in_progress = 1;

    if (g_running_flag) {
        *g_running_flag = 0;
    }

    /* Signal-safe banner */
    const char *name = "?";
    switch (sig) {
    case SIGINT:
        name = "SIGINT";
        break;
    case SIGTERM:
        name = "SIGTERM";
        break;
    case SIGHUP:
        name = "SIGHUP";
        break;
    }
    const char msg1[] = "\nReceived ";
    const char msg2[] = " - shutting down gracefully.\n";
    safe_write(STDOUT_FILENO, msg1, sizeof(msg1) - 1);
    safe_write(STDOUT_FILENO, name, strlen(name));
    safe_write(STDOUT_FILENO, msg2, sizeof(msg2) - 1);
    errno = saved_errno;
}

void pact_signal_install_handlers(volatile sig_atomic_t *running_flag)
{
    g_running_flag = running_flag;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = graceful_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* SIGPIPE: ignore (avoid death on broken stdout/stderr pipe) */
    signal(SIGPIPE, SIG_IGN);

    /* Fatal: write marker + re-raise */
    struct sigaction fa;
    memset(&fa, 0, sizeof(fa));
    fa.sa_handler = fatal_signal_handler;
    sigemptyset(&fa.sa_mask);
    fa.sa_flags = 0;
    sigaction(SIGSEGV, &fa, NULL);
    sigaction(SIGABRT, &fa, NULL);
    sigaction(SIGBUS, &fa, NULL);
    sigaction(SIGILL, &fa, NULL);
    sigaction(SIGFPE, &fa, NULL);

    printf("Signal handlers installed (Ctrl+C for graceful shutdown)\n");
}

void pact_signal_write_clean_marker(int signal_received)
{
    if (g_crash_marker_path[0] == '\0') {
        return;
    }
    FILE *m = fopen(g_crash_marker_path, "w");
    if (!m) {
        return;
    }
    fprintf(m, "ok signal=%d\n", signal_received);
    fclose(m);
}
