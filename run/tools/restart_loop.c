/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech */

/*
 * restart_loop.so - LD_PRELOAD library for long background workloads
 *
 * Restarts the binary in place with execv() when it exits, so that it
 * runs until the runner stops it. The PID stays the same, so the cgroup
 * membership and the runtime's view of the process stay valid.
 *
 * Two hooks start the restart:
 * 1. An atexit() handler. It runs on return from main() and on exit().
 *    The library registers it before the program starts, so it runs after
 *    the atexit() handlers of the program.
 * 2. An exit() hook. It restarts the binary at once. If the restart does
 *    not happen, it calls the real exit() of the C library (found with
 *    dlsym), so that the atexit() handlers run and stdio is flushed.
 *
 * Before each execv(), the library flushes all C stdio streams, so that
 * their buffered output of the finished run is not lost. It cannot flush
 * the buffers of another I/O library (for example the Fortran run-time
 * library of a SPEC binary): output still held there when the binary
 * exits is lost at the restart.
 *
 * PID check: only the process that called __libc_start_main restarts.
 * A fork child inherits saved_argv but has a different PID, so it exits
 * normally. This prevents a fork bomb when a wrapper script (for example
 * cmd.sh) starts the binary.
 *
 * Per-run timing: if RESTART_LOOP_LOG is set, the library appends START
 * and END time stamps to that file as CSV. Pair the events to get the
 * wall time of each run.
 *
 * Compile: gcc -shared -fPIC -o restart_loop.so restart_loop.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static char **saved_argv;
static pid_t saved_pid;
/* Set before the first execv() attempt of this image, so that a failed
 * execv() is not tried again from the atexit() handler. */
static int restart_tried;

static void log_event(const char *event)
{
    const char *logpath = getenv("RESTART_LOOP_LOG");
    if (!logpath) {
        return;
    }

    int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        return;
    }

    /* Write CSV header if file is empty */
    if (lseek(fd, 0, SEEK_END) == 0) {
        static const char hdr[] = "event,timestamp\n";
        if (write(fd, hdr, sizeof(hdr) - 1) < 0) {
            /* ignore write error */
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    char buf[64];
    int len =
        snprintf(buf, sizeof(buf), "%s,%ld.%06ld\n", event, (long)ts.tv_sec, ts.tv_nsec / 1000);
    if (write(fd, buf, len) < 0) {
        /* ignore write error */
    }
    close(fd);
}

static void restart(void)
{
    if (!saved_argv || getpid() != saved_pid || restart_tried) {
        return; /* fork child, no saved_argv, or execv already failed */
    }
    restart_tried = 1;
    log_event("END");
    fflush(NULL);          /* keep the C stdio output of this run */
    lseek(0, 0, SEEK_SET); /* rewind stdin for programs that read their input from it */
    execv("/proc/self/exe", saved_argv);
    /* execv failed: the caller exits normally */
}

int __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv, void (*init)(void),
                      void (*fini)(void), void (*rtld_fini)(void), void *stack_end)
{
    saved_argv = argv;
    saved_pid = getpid();
    typeof(__libc_start_main) *real = dlsym(RTLD_NEXT, "__libc_start_main");
    return real(main, argc, argv, init, fini, rtld_fini, stack_end);
}

__attribute__((constructor)) static void register_restart(void)
{
    log_event("START");
    atexit(restart);
}

void exit(int status)
{
    typeof(exit) *real_exit = dlsym(RTLD_NEXT, "exit");

    restart();
    /* No restart: exit normally (atexit handlers, stdio flush) */
    if (real_exit) {
        real_exit(status);
    }
    fflush(NULL);
    _exit(status); /* only if dlsym found no exit() */
}
