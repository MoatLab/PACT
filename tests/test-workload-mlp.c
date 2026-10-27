/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* Tierce counter rules: per-CPU LLC-miss coverage and the workload MLP
 * (mean of per-core MLP over all of the workload's cores). */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
static ssize_t fake_read(int fd, void *buffer, size_t size);
#define read fake_read
#include "../src/pmu.c"
#undef read
pmu_platform_t g_pmu_platform;

/* One fake per-CPU counting group per fd: leader id 100 + fd, event id fd,
 * and with extra set a second event with id 50 + fd. */
static struct {
    bool fail;
    uint64_t enabled, running, value;
    bool extra;
    uint64_t extra_value;
} group_by_fd[8];

static ssize_t fake_read(int fd, void *buffer, size_t size)
{
    assert(fd >= 0 && fd < 8);
    if (group_by_fd[fd].fail) {
        errno = EIO;
        return -1;
    }
    uint64_t record[] = {group_by_fd[fd].extra ? 3 : 2,
                         group_by_fd[fd].enabled,
                         group_by_fd[fd].running,
                         0,
                         100 + (uint64_t)fd,
                         group_by_fd[fd].value,
                         (uint64_t)fd,
                         group_by_fd[fd].extra_value,
                         50 + (uint64_t)fd};
    size_t bytes = group_by_fd[fd].extra ? sizeof(record) : sizeof(record) - 2 * sizeof(uint64_t);
    assert(size >= bytes);
    memcpy(buffer, record, bytes);
    return (ssize_t)bytes;
}

static void init_cpu(per_cpu_state_t *cs, int cpu, int owner, int fd)
{
    memset(cs, 0, sizeof(*cs));
    cs->cpu_id = cpu;
    cs->owner = owner;
    cs->count_leader.fd = fd;
    cs->count_events[0].fd = fd;
    cs->count_events[0].id = (uint64_t)fd;
    cs->nr_count_events = 1;
    for (int ev = 0; ev < CPU_EVENT_COUNT; ev++) {
        cs->count_slot[ev] = -1;
    }
    cs->count_slot[CPU_EVENT_LLC_MISS_SLOW] = 0;
    cs->cha = -1;
    cs->mlp = g_pmu_platform.mlp_min;
}

static void set_cha(event_group_t *group, bool fresh, uint64_t occupancy, uint64_t cycles)
{
    group->read_valid = fresh;
    group->last_time_running = 0;
    group->time_running = fresh ? 2000000 : 500000;
    group->values[CHA_TOR_OCCUPANCY] = occupancy;
    group->values[CHA_TOR_CYCLES] = cycles;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    g_pmu_platform.mlp_min = 1.0;
    g_pmu_platform.mlp_max = 16.0;

    pact_workload_t workloads[2] = {{.index = 0}, {.index = 1}};
    per_cpu_state_t cpus[4];
    init_cpu(&cpus[0], 2, 0, 1);
    init_cpu(&cpus[1], 3, 0, 2);
    init_cpu(&cpus[2], 4, 0, 3);
    init_cpu(&cpus[3], 5, 1, 4);
    pact_context_t ctx = {.mode = PACT_MODE_TIERCE,
                          .workloads = workloads,
                          .nr_workloads = 2,
                          .cpu_states = cpus,
                          .nr_all_cpus = 4};

    /* CPU 2 counts the whole window; CPU 3 fails; CPU 4 ran half of the
     * window (multiplexed, scaled x2); CPU 5 did not run at all. */
    group_by_fd[1] = (typeof(group_by_fd[1])){false, 1000, 1000, 100};
    group_by_fd[2] = (typeof(group_by_fd[2])){true, 0, 0, 0};
    group_by_fd[3] = (typeof(group_by_fd[3])){false, 1000, 500, 40};
    group_by_fd[4] = (typeof(group_by_fd[4])){false, 1000, 0, 70};
    read_cpu_counting_groups(&ctx);
    assert(cpus[0].count_valid && cpus[0].llc_misses == 100);
    assert(!cpus[1].count_valid && cpus[1].llc_misses == 0);
    assert(cpus[2].count_valid && cpus[2].llc_misses == 80);
    assert(!cpus[3].count_valid);
    assert(workloads[0].stats.llc_misses_slow == 180);
    assert(workloads[1].stats.llc_misses_slow == 0);

    /* The next window is the difference of the cumulative counts and clocks. */
    group_by_fd[1] = (typeof(group_by_fd[1])){false, 3000, 3000, 130};
    group_by_fd[2].fail = false;
    group_by_fd[2] = (typeof(group_by_fd[2])){false, 2000, 2000, 5};
    read_cpu_counting_groups(&ctx);
    assert(cpus[0].count_valid && cpus[0].llc_misses == 30);
    assert(cpus[1].count_valid && cpus[1].llc_misses == 5);
    assert(workloads[0].stats.llc_misses_slow == 35);

    /* Workload MLP: every core starts at mlp_min. */
    cha_pmu_info_t *chas = ctx.cha_pmus;
    ctx.nr_cha = 4;
    for (int c = 0; c < 4; c++) {
        cpus[c].cha = c;
        set_cha(&chas[c].group, false, 0, 0);
    }
    calculate_workload_mlp(&ctx);
    assert(workloads[0].mlp == 1.0 && workloads[1].mlp == 1.0);

    /* Fresh readings: 400/100 = 4, 900/300 = 3, an idle core (zero traffic)
     * gets mlp_min and stays in the mean. */
    set_cha(&chas[0].group, true, 400, 100);
    set_cha(&chas[1].group, true, 900, 300);
    set_cha(&chas[2].group, true, 0, 0);
    set_cha(&chas[3].group, true, 1000, 10); /* clamped to mlp_max */
    calculate_workload_mlp(&ctx);
    assert(fabs(workloads[0].mlp - (4.0 + 3.0 + 1.0) / 3.0) < 1e-12);
    assert(workloads[1].mlp == 16.0);

    /* A stale core (short run or failed read) keeps its last value. */
    set_cha(&chas[0].group, false, 0, 0);
    set_cha(&chas[1].group, true, 600, 300);
    calculate_workload_mlp(&ctx);
    assert(cpus[0].mlp == 4.0 && cpus[1].mlp == 2.0);
    assert(fabs(workloads[0].mlp - (4.0 + 2.0 + 1.0) / 3.0) < 1e-12);

    /* All cores idle: the workload gets mlp_min. */
    for (int c = 0; c < 3; c++) {
        set_cha(&chas[c].group, true, 0, 5);
    }
    calculate_workload_mlp(&ctx);
    assert(workloads[0].mlp == 1.0);

    /* CAT feed: the FG's (workload 0) CPUs add their window counts and
     * clocks, unscaled, to running sums; a failed FG read is flagged. The
     * groups run free, so each window is the difference to the last read. */
    per_cpu_state_t fgcpu;
    init_cpu(&fgcpu, 6, 0, 5);
    fgcpu.count_events[1].fd = 5;
    fgcpu.count_events[1].id = 55;
    fgcpu.nr_count_events = 2;
    fgcpu.count_slot[CPU_EVENT_INSTRUCTIONS] = 1;
    pact_context_t fctx = {.mode = PACT_MODE_TIERCE,
                           .workloads = workloads,
                           .nr_workloads = 2,
                           .cpu_states = &fgcpu,
                           .nr_all_cpus = 1};
    fctx.cat.feed.active = true;
    group_by_fd[5] = (typeof(group_by_fd[5])){false, 1000, 500, 10, true, 7000};
    read_cpu_counting_groups(&fctx);
    assert(fgcpu.count_valid && fgcpu.llc_misses == 20);         /* LLC scaled x2 */
    assert(fctx.cat.feed.count[CPU_EVENT_INSTRUCTIONS] == 7000); /* feed unscaled */
    assert(fctx.cat.feed.count[CPU_EVENT_LLC_MISS_SLOW] == 10);
    assert(fctx.cat.feed.time_enabled == 1000 && fctx.cat.feed.time_running == 500);
    group_by_fd[5] = (typeof(group_by_fd[5])){false, 3000, 2500, 14, true, 16000};
    read_cpu_counting_groups(&fctx);
    assert(fctx.cat.feed.count[CPU_EVENT_INSTRUCTIONS] == 16000);
    assert(fctx.cat.feed.time_enabled == 3000 && fctx.cat.feed.time_running == 2500);
    assert(!fctx.cat.feed.read_failed);
    group_by_fd[5].fail = true;
    read_cpu_counting_groups(&fctx);
    assert(fctx.cat.feed.read_failed && fctx.cat.feed.count[CPU_EVENT_INSTRUCTIONS] == 16000);
    /* Another workload's CPU does not feed the controller. */
    fctx.cat.feed.read_failed = false;
    group_by_fd[5].fail = false;
    fgcpu.owner = 1;
    group_by_fd[5].enabled = 4000;
    group_by_fd[5].running = 3500;
    read_cpu_counting_groups(&fctx);
    assert(fctx.cat.feed.count[CPU_EVENT_INSTRUCTIONS] == 16000);
    return 0;
}
