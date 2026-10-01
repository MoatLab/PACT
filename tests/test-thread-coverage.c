/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

long fake_perf_syscall(long number, ...);
int fake_perf_ioctl(int fd, unsigned long request, ...);
#define syscall fake_perf_syscall
#define ioctl fake_perf_ioctl
#include "../src/pmu.c"
#undef syscall
#undef ioctl

pmu_platform_t g_pmu_platform;
static struct {
    pid_t tid;
    int fd;
    bool inherit;
} opened[64];
static size_t nr_opened;
static atomic_int worker_tid;
static atomic_bool finished;
static pid_t fail_tid;

long fake_perf_syscall(long number, ...)
{
    assert(number == __NR_perf_event_open && nr_opened < 64);
    va_list args;
    va_start(args, number);
    struct perf_event_attr *attr = va_arg(args, struct perf_event_attr *);
    pid_t tid = va_arg(args, int);
    int cpu = va_arg(args, int);
    va_end(args);
    assert(tid > 0 && cpu == -1);
    if (tid == fail_tid && attr->type == PERF_TYPE_RAW) {
        errno = EACCES;
        return -1;
    }
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    opened[nr_opened].tid = tid;
    opened[nr_opened].fd = fd;
    opened[nr_opened++].inherit = attr->inherit;
    return fd;
}

int fake_perf_ioctl(int fd, unsigned long request, ...)
{
    if (request == PERF_EVENT_IOC_ID) {
        va_list args;
        va_start(args, request);
        uint64_t *id = va_arg(args, uint64_t *);
        *id = (uint64_t)fd;
        va_end(args);
    }
    return 0;
}

static void *worker(void *unused)
{
    (void)unused;
    atomic_store(&worker_tid, syscall(SYS_gettid));
    while (!atomic_load(&finished)) {
        usleep(100);
    }
    return NULL;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, worker, NULL));
    while (!atomic_load(&worker_tid)) {
        usleep(100);
    }
    pact_workload_t workload = {.target_pid = getpid()};
    assert(setup_workload_counting_events(&workload) == 0);
    size_t worker_events = 0;
    for (size_t i = 0; i < nr_opened; i++) {
        worker_events += opened[i].tid == atomic_load(&worker_tid);
    }
    fprintf(stderr, "existing worker events=%zu required=%d\n", worker_events,
            CORE_EVENT_COUNT + 1);
    assert(worker_events == CORE_EVENT_COUNT + 1);
    assert(nr_opened == 2 * (CORE_EVENT_COUNT + 1));
    for (size_t i = 0; i < nr_opened; i++) {
        assert(!opened[i].inherit);
    }
    pid_t first_tid = atomic_load(&worker_tid);
    size_t before = nr_opened;
    assert(setup_workload_counting_events(&workload) == 0);
    assert(nr_opened == before); /* Reuse counters across stable windows. */
    pthread_t later;
    atomic_store(&worker_tid, 0);
    assert(!pthread_create(&later, NULL, worker, NULL));
    while (!atomic_load(&worker_tid)) {
        usleep(100);
    }
    pid_t later_tid = atomic_load(&worker_tid);
    assert(!workload_covers_tid(&workload, later_tid));
    assert(setup_workload_counting_events(&workload) == 0);
    assert(nr_opened == before + CORE_EVENT_COUNT + 1);
    assert(workload_covers_tid(&workload, first_tid));
    assert(workload_covers_tid(&workload, later_tid));
    atomic_store(&finished, true);
    assert(!pthread_join(thread, NULL));
    assert(!pthread_join(later, NULL));
    assert(setup_workload_counting_events(&workload) == 0);
    assert(workload.nr_threads == 1);
    assert(!workload_covers_tid(&workload, first_tid));
    assert(!workload_covers_tid(&workload, later_tid));
    cleanup_workload_counting_events(&workload);
    assert(!workload.threads && workload.nr_threads == 0);
    /* Failed member creation must close its leader and report failure. */
    atomic_store(&finished, false);
    atomic_store(&worker_tid, 0);
    assert(!pthread_create(&thread, NULL, worker, NULL));
    while (!atomic_load(&worker_tid)) {
        usleep(100);
    }
    fail_tid = atomic_load(&worker_tid);
    assert(setup_workload_counting_events(&workload) < 0);
    cleanup_workload_counting_events(&workload);
    for (size_t i = 0; i < nr_opened; i++) {
        assert(fcntl(opened[i].fd, F_GETFD) == -1 && errno == EBADF);
    }
    atomic_store(&finished, true);
    assert(!pthread_join(thread, NULL));
    return 0;
}
