/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
static ssize_t fake_read(int fd, void *buffer, size_t size);
#define read fake_read
#include "../src/pmu.c"
#undef read
pmu_platform_t g_pmu_platform;
static bool fail_read, malformed;
static uint64_t enabled = 2000000, running = 2000000;
static ssize_t fake_read(int fd, void *buffer, size_t size)
{
    (void)fd;
    if (fail_read) {
        errno = EIO;
        return -1;
    }
    uint64_t record[] = {2, enabled, running, 100, 11, 50, malformed ? 11 : 12};
    assert(size >= sizeof(record));
    memcpy(buffer, record, sizeof(record));
    return sizeof(record);
}
int main(void)
{
    event_group_t group = {.counters_used = 2, .fds = {1, 2}, .ids = {11, 12}};
    assert(read_pmu_event_group(&group) == 0);
    assert(group.values[0] == 100 && group.values[1] == 50);
    fail_read = true;
    assert(read_pmu_event_group(&group) < 0);
    assert(group.values[0] == 0 && group.values[1] == 0);
    assert(!group.read_valid);
    fail_read = false;
    enabled = running = 6000000;
    assert(read_pmu_event_group(&group) < 0); /* Restore the missed boundary. */
    assert(!group.read_valid && !group.needs_resync);
    enabled = running = 8000000;
    assert(read_pmu_event_group(&group) == 0);
    assert(group.time_running - group.last_time_running == 2000000);
    scale_multiplexed_events(&group);
    assert(group.values[0] == 100 && group.values[1] == 50);

    malformed = true;
    assert(read_pmu_event_group(&group) < 0);
    assert(!group.read_valid && group.values[0] == 0 && group.values[1] == 0);
    malformed = false;
    enabled = running = 10000000;
    assert(read_pmu_event_group(&group) < 0);
    enabled = 14000000;
    running = 12000000;
    assert(read_pmu_event_group(&group) == 0);
    scale_multiplexed_events(&group);
    assert(group.values[0] == 200 && group.values[1] == 100);

    enabled = running = 2000000; /* Counter-time rollback cannot underflow. */
    assert(read_pmu_event_group(&group) < 0);
    assert(!group.read_valid && !group.values[0]);
    enabled = running = 4000000;
    assert(read_pmu_event_group(&group) == 0);
    enabled = 5000000;
    running = 6000000;
    assert(read_pmu_event_group(&group) < 0);
    assert(!group.read_valid && !group.values[0]);

    pact_workload_t workload = {.nr_cha = 2, .counters_valid = true};
    cha_pmu_info_t *chas = workload.cha_pmus;
    g_pmu_platform.mlp_min = 1;
    g_pmu_platform.mlp_max = 32;
    enabled = running = 8000000;
    group = (event_group_t){.counters_used = 2, .fds = {1, 2}, .ids = {11, 12}};
    assert(read_pmu_event_group(&group) == 0);
    for (int i = 0; i < 2; i++) {
        chas[i].group_fast = chas[i].group_slow = group;
    }
    calculate_workload_mlp(&workload);
    assert(workload.counters_valid && workload.workload_mlp_fast == 2.0);
    assert(workload.workload_mlp_slow == 2.0);
    /* One stale group must invalidate the entire attribution window. */
    chas[1].group_fast.read_valid = false;
    calculate_workload_mlp(&workload);
    assert(!workload.counters_valid);
    chas[1].group_fast = group;
    chas[1].group_slow.time_running = chas[1].group_slow.last_time_running + 500000;
    workload.counters_valid = true;
    calculate_workload_mlp(&workload);
    assert(!workload.counters_valid);
}
