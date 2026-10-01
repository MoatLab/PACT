/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* utils.c — small stateless utility functions.
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
        log_error("pin_to_cpu", "Failed to set affinity to CPU %d: %s", cpu, strerror(errno));
        return -1;
    }
    log_info("pin_to_cpu", "Pinned to CPU %d", cpu);
    return 0;
}

bool pact_check_all_targets_exited(pact_context_t *ctx)
{
    if (ctx->target_pidfd < 0) {
        return false;
    }
    struct pollfd target = {.fd = ctx->target_pidfd, .events = POLLIN};
    int result = poll(&target, 1, 0);
    if ((result < 0 && errno != EINTR) || (target.revents & POLLNVAL)) {
        log_error("pact_check_all_targets_exited", "Cannot query workload pidfd");
        ctx->sampling_failed = true;
        ctx->running = false;
    }
    return result > 0 && (target.revents & (POLLIN | POLLHUP));
}
