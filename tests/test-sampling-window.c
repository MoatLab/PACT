/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include "../src/pebs-aggregator.c"
#include "../src/tsc.h"
#include <assert.h>
static pact_context_t *active;
static uint64_t restarted;
static bool exit_after_refresh, target_exited;
static struct perf_event_mmap_page *sample_ring;
void *mco_get_user_data(mco_coro *co)
{
    (void)co;
    return active;
}
mco_result mco_yield(mco_coro *co)
{
    (void)co;
    active->running = false;
    return MCO_SUCCESS;
}
void stop_pmu_perf_events(pact_context_t *ctx)
{
    (void)ctx;
}
void read_pmu_counting_events(pact_context_t *ctx)
{
#ifndef PUBLIC_BASELINE
    ctx->workload->counters_valid = true;
#else
    (void)ctx;
#endif
}
void start_pmu_perf_events(pact_context_t *ctx)
{
    (void)ctx;
    /* Restarting before this drain mixes next-window samples with old counts. */
    assert(sample_ring->data_tail == sample_ring->data_head);
    restarted = rdtsc();
}
#ifndef PUBLIC_BASELINE
int setup_workload_counting_events(pact_workload_t *wl)
{
    (void)wl;
    if (exit_after_refresh) {
        target_exited = true;
        errno = ESRCH;
        return -1;
    }
    return 0;
}
#endif
bool workload_covers_tid(const pact_workload_t *wl, pid_t tid)
{
    (void)wl;
    (void)tid;
    return true;
}
bool pact_check_all_targets_exited(pact_context_t *ctx)
{
    (void)ctx;
    return target_exited;
}
int main(void)
{
    per_cpu_state_t cpu = {.fd_pebs = 1};
    cpu.pebs_mmap = calloc(1, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
    assert(cpu.pebs_mmap);
    sample_ring = cpu.pebs_mmap;
    struct {
        struct perf_event_header header;
        uint32_t pid, tid;
        uint64_t addr;
    } record = {
        .header = {.type = PERF_RECORD_SAMPLE}, .pid = getpid(), .tid = getpid(), .addr = 4096};
    record.header.size = sizeof(record);
    memcpy((char *)cpu.pebs_mmap + PAGE_SIZE, &record, sizeof(record));
    sample_ring->data_head = sizeof(record);
    pact_workload_t wl = {.workload_mlp_slow = 1, .target_pid = getpid()};
    pact_context_t ctx = {
        .workload = &wl, .running = true, .sampling_interval_ms = 10, .tsc_freq_hz = 1000000000};
    active = &ctx;
    ctx.pac_update_ring = ring_buffer_uint64_create(16);
    pebs_aggregator_t *agg = pebs_aggregator_create(&cpu, 1, 1);
    ctx.pebs_aggregator = agg;
    agg->pact_ctx = &ctx;
    /* A scheduler deadline already expired while counters were disabled. */
    ctx.timing[CORO_TYPE_PEBS].next_tsc = 1;
    pebs_aggregator_coroutine(NULL);
    printf("expired_deadline=%lu restart=%lu required_interval=%lu\n",
           ctx.timing[CORO_TYPE_PEBS].next_tsc, restarted, ms_to_tsc(&ctx, 10));
    assert(ctx.timing[CORO_TYPE_PEBS].next_tsc >= restarted + ms_to_tsc(&ctx, 10));
#ifndef PUBLIC_BASELINE
    /* Exit can become visible between the first check and task refresh. */
    exit_after_refresh = true;
    restarted = 0;
    ctx.running = true;
    pebs_aggregator_coroutine(NULL);
    assert(!ctx.running && !ctx.sampling_failed && restarted == 0);
#endif
    pebs_aggregator_destroy(agg);
    ring_buffer_uint64_destroy(ctx.pac_update_ring);
    free(cpu.pebs_mmap);
}
