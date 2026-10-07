/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
static int inject, saw_task;
static struct dirent *scan_readdir(DIR *dir)
{
    if (inject && saw_task) {
        errno = EIO;
        return NULL;
    }
    struct dirent *entry = readdir(dir);
    if (entry && entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
        saw_task = 1;
    }
    /* A successful read can leave stale errno for the next iteration. */
    if (entry) {
        errno = EIO;
    }
    return entry;
}
#define readdir scan_readdir
#include "pmu.c"
#undef readdir
int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    inject = atoi(argv[1]);
    pact_workload_t wl = {.target_pid = getpid()};
    wl.threads = calloc(1, sizeof(*wl.threads));
    if (!wl.threads) {
        return 2;
    }
    wl.nr_threads = 1;
    wl.threads[0].tid = getpid();
    if (thread_start_time(getpid(), getpid(), &wl.threads[0].start_time)) {
        return 2;
    }
    wl.threads[0].leader.fd = -1;
    for (int j = 0; j < CORE_EVENT_COUNT; j++) {
        wl.threads[0].events[j].fd = -1;
    }
    int result = setup_workload_counting_events(&wl), saved = errno;
    printf("inject=%d result=%d errno=%d retained=%zu\n", inject, result, saved, wl.nr_threads);
    int failed = wl.nr_threads != 1 || (inject ? result != -1 || saved != EIO : result != 0);
    cleanup_workload_counting_events(&wl);
    return failed;
}
