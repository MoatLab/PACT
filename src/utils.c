/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* utils.c: small stateless utility functions.
 * _GNU_SOURCE is supplied by the Makefile (-D_GNU_SOURCE); needed here for
 * sched_setaffinity. */

#include <errno.h>
#include <sched.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* pid_t */
#include <time.h>
#include <unistd.h> /* usleep */

#include "pact.h" /* pac_metadata_t, pact_context_t */
#include "tsc.h"  /* rdtsc */
#include "utils.h"
#include "error.h"

uint64_t pact_detect_tsc_frequency(void)
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t tsc_start = rdtsc();
    usleep(10000); /* 10 ms calibration window */
    uint64_t tsc_end = rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t elapsed_ns =
        (end.tv_sec - start.tv_sec) * 1000000000ULL + (end.tv_nsec - start.tv_nsec);
    return (tsc_end - tsc_start) * 1000000000ULL / elapsed_ns;
}

int pact_pin_to_cpu(int cpu)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        int error = errno; /* the caller reports it */
        log_error("pin_to_cpu", "Failed to set affinity to CPU %d: %s", cpu, strerror(error));
        errno = error;
        return -1;
    }
    log_info("pin_to_cpu", "Pinned to CPU %d", cpu);
    return 0;
}

void pact_mark_workload_exited(pact_workload_t *wl)
{
    if (wl->exited) {
        return;
    }
    wl->exited = true;
    log_info("pact_check_all_targets_exited", "Workload %d (PID %d) has exited", wl->index,
             (int)wl->target_pid);
}

bool pact_workload_exited(pact_context_t *ctx, pact_workload_t *wl)
{
    if (wl->exited || wl->pidfd < 0) {
        return wl->exited;
    }
    struct pollfd target = {.fd = wl->pidfd, .events = POLLIN};
    int result = poll(&target, 1, 0);
    if ((result < 0 && errno != EINTR) || (target.revents & POLLNVAL)) {
        log_error("pact_check_all_targets_exited", "Cannot query workload pidfd");
        ctx->sampling_failed = true;
        ctx->running = false;
        return false;
    }
    if (result > 0 && (target.revents & (POLLIN | POLLHUP))) {
        pact_mark_workload_exited(wl);
    }
    return wl->exited;
}

bool pact_check_all_targets_exited(pact_context_t *ctx)
{
    if (ctx->nr_workloads <= 0) {
        return false;
    }
    bool all_exited = true;
    for (int i = 0; i < ctx->nr_workloads; i++) {
        if (!pact_workload_exited(ctx, &ctx->workloads[i])) {
            all_exited = false;
        }
    }
    return all_exited;
}

int pact_parse_cpu_list(const char *text, uint64_t *mask)
{
    uint64_t cpus = 0;
    const char *p = text;
    if (*p == '\0') {
        return -1;
    }
    while (*p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        char *end = NULL;
        errno = 0;
        unsigned long first = strtoul(p, &end, 10);
        unsigned long last = first;
        if (errno != 0) {
            return -1;
        }
        p = end;
        if (*p == '-') {
            p++;
            if (*p < '0' || *p > '9') {
                return -1;
            }
            last = strtoul(p, &end, 10);
            if (errno != 0) {
                return -1;
            }
            p = end;
        }
        if (first > last || last > 63) {
            return -1;
        }
        for (unsigned long cpu = first; cpu <= last; cpu++) {
            cpus |= 1ULL << cpu;
        }
        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return -1;
            }
        } else if (*p != '\0') {
            return -1;
        }
    }
    *mask = cpus;
    return 0;
}
