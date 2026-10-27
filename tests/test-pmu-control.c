/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
int fake_ioctl(int fd, unsigned long request, ...);
#define ioctl fake_ioctl
#include "../src/pmu.c"
#undef ioctl
pmu_platform_t g_pmu_platform;
static int failed_fd = 10;
static unsigned long failed_request = PERF_EVENT_IOC_RESET;
static unsigned int disabled;
int fake_ioctl(int fd, unsigned long request, ...)
{
    assert(fd >= 0); /* Offline core slots must not issue control calls. */
    va_list args;
    va_start(args, request);
    assert(va_arg(args, int) == PERF_IOC_FLAG_GROUP);
    va_end(args);
    if (request == PERF_EVENT_IOC_DISABLE && fd >= 10 && fd <= 13) {
        disabled |= 1u << (fd - 10);
    }
    if (fd == failed_fd && request == failed_request) {
        errno = EIO;
        return -1;
    }
    return 0;
}
int main(void)
{
    per_cpu_state_t cpus[3] = {{.leader.fd = 10}, {.leader.fd = 12}, {.leader.fd = -1}};
    thread_perf_t thread = {.tid = 123, .leader.fd = 13};
    pact_workload_t workload = {.nr_threads = 1, .threads = &thread};
    pact_context_t ctx = {.cpu_states = cpus,
                          .nr_all_cpus = 3,
                          .workloads = &workload,
                          .nr_workloads = 1,
                          .nr_cha = 1,
                          .running = true};
    ctx.cha_pmus[0].group.fds[0] = 11;
    failed_fd = -1;
    start_pmu_perf_events(&ctx);
    assert(!ctx.sampling_failed && ctx.running && !disabled);
    stop_pmu_perf_events(&ctx);
    assert(!ctx.sampling_failed && ctx.running && disabled == 15);
    unsigned long requests[] = {PERF_EVENT_IOC_RESET, PERF_EVENT_IOC_ENABLE,
                                PERF_EVENT_IOC_DISABLE};
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        for (int fd = 10; fd <= 13; fd++) {
            ctx.sampling_failed = false;
            ctx.running = true;
            disabled = 0;
            failed_fd = fd;
            failed_request = requests[i];
            if (failed_request == PERF_EVENT_IOC_DISABLE) {
                stop_pmu_perf_events(&ctx);
            } else {
                start_pmu_perf_events(&ctx);
            }
            assert(ctx.sampling_failed && !ctx.running);
            assert(disabled == 15); /* A failure cannot skip later cleanup groups. */
        }
    }

    /* Tierce mode: CHA (fd 11) stops; PEBS (fds 10, 12) is flushed and
     * re-enabled, never reset. A failure still stops every group. */
    per_cpu_state_t tcpus[3] = {{.leader.fd = 10, .count_leader.fd = -1},
                                {.leader.fd = 12, .count_leader.fd = -1},
                                {.leader.fd = -1, .count_leader.fd = -1}};
    ctx.cpu_states = tcpus;
    ctx.mode = PACT_MODE_TIERCE;
    ctx.sampling_failed = false;
    ctx.running = true;
    disabled = 0;
    failed_fd = 10;
    failed_request = PERF_EVENT_IOC_RESET;
    start_pmu_perf_events(&ctx);
    assert(!ctx.sampling_failed && ctx.running); /* no reset reached fd 10 */
    stop_pmu_perf_events(&ctx);
    assert(!ctx.sampling_failed && ctx.running && disabled == 7);
    disabled = 0;
    failed_request = PERF_EVENT_IOC_ENABLE;
    start_pmu_perf_events(&ctx);
    assert(ctx.sampling_failed && !ctx.running && disabled == 7);
}
