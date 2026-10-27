/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* sighandler.h: signal handling + crash marker for PACT runtime.
 *
 *
 * USAGE
 *
 *   1. Optionally set crash marker path before installing handlers:
 *        pact_signal_set_crash_marker_path("/path/to/marker");
 *
 *   2. Install handlers (call once during single-threaded startup):
 *        pact_signal_install_handlers(&g_pact->running);
 *      - SIGINT/SIGTERM/SIGHUP: graceful shutdown (sets *running to 0)
 *      - SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE: write crash marker + re-raise
 *      - SIGPIPE: ignored
 *
 *   3. At clean exit, write the "ok" marker:
 *        pact_signal_write_clean_marker(received_signal);
 *
 *   4. Read the received-signal value (for shutdown-cause reporting):
 *        int sig = pact_signal_received();
 */

#ifndef PACT_SIGNAL_H
#define PACT_SIGNAL_H

#include <signal.h>
#include <stdbool.h>

/* Set crash-marker file path. If non-empty, fatal signal handlers will
 * write "crash sig=NN\n" to this path before re-raising the signal
 * (preserving the core dump). NULL/empty disables. */
void pact_signal_set_crash_marker_path(const char *path);

/* Install signal handlers. `running_flag` (must stay valid until
 * pact_signal_clear_running_flag()) is
 * set to 0 on SIGINT/SIGTERM/SIGHUP for graceful shutdown.
 * Pass NULL if you don't want graceful-shutdown wiring (handlers will
 * still record the signal). */
void pact_signal_install_handlers(volatile sig_atomic_t *running_flag);

/* Detach the handler from the running flag before the flag is freed. */
void pact_signal_clear_running_flag(void);

/* Last received signal (SIGINT/SIGTERM/SIGHUP), or 0 if none. */
int pact_signal_received(void);

/* Write the clean-exit marker. signal_received is the value returned
 * by pact_signal_received(): pass 0 if the process is exiting normally. */
void pact_signal_write_clean_marker(int signal_received);

#endif /* PACT_SIGNAL_H */
