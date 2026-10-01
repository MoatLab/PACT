/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include "../src/perf.c"
#include <assert.h>

static bool available, started, workload_failed, control_failed;
int discover_cha_pmus(cha_pmu_info_t *pmus, int *nr, uint64_t mask)
{
    (void)pmus;
    (void)mask;
    *nr = 1;
    return 0;
}
int setup_pmu_cha_perf_events(cha_pmu_info_t *pmus, int *nr)
{
    (void)pmus;
    (void)nr;
    return 0;
}
int setup_dummy_leader_event(perf_event_t *event, pid_t pid, int cpu)
{
    (void)pid;
    (void)cpu;
    event->fd = 1;
    return 0;
}
int setup_pebs_event(per_cpu_state_t *cpu, pid_t pid, int id)
{
    (void)pid;
    (void)id;
    if (!available) {
        return -1;
    }
    cpu->fd_pebs = 2;
    cpu->pebs_mmap = cpu;
    return 0;
}
int setup_workload_counting_events(pact_workload_t *wl)
{
    (void)wl;
    return workload_failed ? -1 : 0;
}
void start_pmu_perf_events(pact_context_t *ctx)
{
    ctx->sampling_failed = control_failed;
    started = true;
}
int main(void)
{
    per_cpu_state_t cpu;
    init_per_cpu_state(&cpu);
    pact_workload_t wl = {0};
    pact_context_t ctx = {.workload = &wl, .cpu_states = &cpu, .nr_all_cpus = 1};
    assert(setup_pact_perf_events(&ctx) < 0);
    assert(!started);
    available = true;
    assert(setup_pact_perf_events(&ctx) == 0);
    assert(started);
    started = false;
    workload_failed = true;
    assert(setup_pact_perf_events(&ctx) < 0);
    assert(ctx.sampling_failed && !started);
    ctx.sampling_failed = false;
    workload_failed = false;
    control_failed = true;
    assert(setup_pact_perf_events(&ctx) < 0);
    assert(ctx.sampling_failed && started);
}
