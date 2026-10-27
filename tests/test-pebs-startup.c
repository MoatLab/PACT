/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include "../src/perf.c"
#include <assert.h>

pmu_platform_t g_pmu_platform;
static bool available, started, workload_failed, control_failed, count_failed;
static int cha_core, cha_fd = 5;
int discover_cha_pmus(cha_pmu_info_t *pmus, int *nr, uint64_t mask)
{
    (void)mask;
    pmus[0].core_id = cha_core;
    pmus[0].group.fds[0] = cha_fd;
    *nr = 1;
    return 0;
}
static int extra_failed_cpu = -1;
static uint32_t extra_seen[2];
static int extra_closed;
int setup_cpu_counting_group(per_cpu_state_t *cs, uint32_t extra_events)
{
    if (count_failed) {
        return -1;
    }
    extra_seen[cs->owner] = extra_events;
    return cs->cpu_id == extra_failed_cpu ? 1 : 0;
}
void close_cpu_extra_events(per_cpu_state_t *cs)
{
    (void)cs;
    extra_closed++;
}
int setup_pmu_cha_perf_events(cha_pmu_info_t *pmus, int *nr, bool tid_filter)
{
    (void)pmus;
    (void)nr;
    (void)tid_filter;
    return 0;
}
int setup_dummy_leader_event(perf_event_t *event, pid_t pid, int cpu)
{
    (void)pid;
    (void)cpu;
    event->fd = 1;
    return 0;
}
int setup_mem_loads_aux_leader(perf_event_t *event, int cpu)
{
    (void)cpu;
    event->fd = 1;
    return 0;
}
static bool pebs_fails_on_cpu2;
int setup_pebs_event(per_cpu_state_t *cpu, pid_t pid, int id, bool use_ldlat)
{
    (void)pid;
    (void)id;
    (void)use_ldlat;
    if (!available || (pebs_fails_on_cpu2 && cpu->cpu_id == 2)) {
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
    pact_context_t ctx = {
        .workloads = &wl, .nr_workloads = 1, .cpu_states = &cpu, .nr_all_cpus = 1};
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

    /* PEBS fails on one CPU of two: the run stops before counting starts. */
    control_failed = false;
    per_cpu_state_t pcpu[2];
    init_per_cpu_state(&pcpu[0]);
    init_per_cpu_state(&pcpu[1]);
    pcpu[0].cpu_id = 1;
    pcpu[1].cpu_id = 2;
    pact_context_t pctx = {
        .workloads = &wl, .nr_workloads = 1, .cpu_states = pcpu, .nr_all_cpus = 2};
    pebs_fails_on_cpu2 = true;
    started = false;
    assert(setup_pact_perf_events(&pctx) < 0);
    assert(!started && pcpu[0].fd_pebs >= 0 && pcpu[1].fd_pebs < 0);
    pebs_fails_on_cpu2 = false;
    assert(setup_pact_perf_events(&pctx) == 0 && started);

    /* Tierce: every CPU needs its paired CHA with an open TOR group and its
     * own counting group; otherwise the run stops before counting starts. */
    control_failed = false;
    per_cpu_state_t tcpu;
    init_per_cpu_state(&tcpu);
    tcpu.cpu_id = 3;
    pact_context_t tctx = {.mode = PACT_MODE_TIERCE,
                           .workloads = &wl,
                           .nr_workloads = 1,
                           .cpu_states = &tcpu,
                           .nr_all_cpus = 1,
                           .all_cpus_mask = 1ULL << 3};
    started = false;
    cha_core = 4;
    assert(setup_pact_perf_events(&tctx) < 0);
    assert(tctx.sampling_failed && !started);
    tctx.sampling_failed = false;
    cha_core = 3;
    cha_fd = -1;
    assert(setup_pact_perf_events(&tctx) < 0 && !started);
    tctx.sampling_failed = false;
    cha_fd = 5;
    count_failed = true;
    assert(setup_pact_perf_events(&tctx) < 0 && !started);
    tctx.sampling_failed = false;
    count_failed = false;
    tcpu.owner = 0;
    assert(setup_pact_perf_events(&tctx) == 0);
    assert(started && tcpu.cha == 0);

    /* The LLC way controller's events join only the FG's (workload 0)
     * groups. If one does not open, they close on every FG CPU and the run
     * continues. */
    per_cpu_state_t two[2];
    init_per_cpu_state(&two[0]);
    init_per_cpu_state(&two[1]);
    two[0].cpu_id = 3;
    two[0].owner = 0;
    two[1].cpu_id = 4;
    two[1].owner = 1;
    pact_workload_t wls[2] = {{0}};
    pact_context_t cctx = {.mode = PACT_MODE_TIERCE,
                           .workloads = wls,
                           .nr_workloads = 2,
                           .cpu_states = two,
                           .nr_all_cpus = 2,
                           .all_cpus_mask = (1ULL << 3) | (1ULL << 4)};
    cctx.cat.events = 1u << CPU_EVENT_INSTRUCTIONS;
    cha_core = 3;                              /* both CPUs pair with the one stub CHA below */
    assert(setup_pact_perf_events(&cctx) < 0); /* CPU 4 has no CHA */
    cctx.sampling_failed = false;
    cctx.nr_all_cpus = 1;
    extra_failed_cpu = 3;
    assert(setup_pact_perf_events(&cctx) == 0);
    assert(extra_seen[0] == cctx.cat.events && extra_closed == 1);
}
