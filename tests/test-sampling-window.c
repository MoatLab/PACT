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
    (void)ctx;
}
void start_pmu_perf_events(pact_context_t *ctx)
{
    (void)ctx;
    /* Restarting before this drain mixes next-window samples with old counts. */
    assert(sample_ring->data_tail == sample_ring->data_head);
    restarted = rdtsc();
}
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
bool pact_workload_exited(pact_context_t *ctx, pact_workload_t *wl)
{
    (void)ctx;
    wl->exited = target_exited;
    return target_exited;
}
void cleanup_workload_counting_events(pact_workload_t *wl)
{
    (void)wl;
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
    pact_workload_t wl = {.target_pid = getpid()};
    pac_domain_t domain = {0};
    pact_context_t ctx = {.workloads = &wl,
                          .nr_workloads = 1,
                          .domains = &domain,
                          .nr_domains = 1,
                          .global_mlp = 1,
                          .running = true,
                          .sampling_interval_ms = 10,
                          .tsc_freq_hz = 1000000000};
    active = &ctx;
    ctx.pac_update_ring = ring_buffer_pac_update_create(16);
    pebs_aggregator_t *agg = pebs_aggregator_create(&cpu, 1, 1);
    ctx.pebs_aggregator = agg;
    agg->pact_ctx = &ctx;
    /* Timestamped records must retain the address even across ring wrap. */
    const size_t bytes = PERF_BUFFER_PAGES * PAGE_SIZE;
    const size_t starts[] = {0, bytes - 4, bytes - 16};
    for (size_t n = 0; n < sizeof(starts) / sizeof(starts[0]); n++) {
        for (size_t i = 0; i < sizeof(record); i++) {
            ((char *)cpu.pebs_mmap + PAGE_SIZE)[(starts[n] + i) % bytes] = ((char *)&record)[i];
        }
        sample_ring->data_tail = starts[n];
        sample_ring->data_head = starts[n] + sizeof(record);
        staged_sample_t event = {0};
        assert(read_cpu_pebs_events(agg, &cpu, &event, 1) == 1);
        assert(event.page == (record.addr & PAGE_MASK) && event.workload == 0);
        assert(sample_ring->data_tail == sample_ring->data_head);
    }
    memcpy((char *)cpu.pebs_mmap + PAGE_SIZE, &record, sizeof(record));
    sample_ring->data_tail = 0;
    sample_ring->data_head = sizeof(record);
    /* A scheduler deadline already expired while counters were disabled. */
    ctx.timing[CORO_TYPE_PEBS].next_tsc = 1;
    pebs_aggregator_coroutine(NULL);
    printf("expired_deadline=%lu restart=%lu required_interval=%lu\n",
           ctx.timing[CORO_TYPE_PEBS].next_tsc, restarted, ms_to_tsc(&ctx, 10));
    assert(ctx.timing[CORO_TYPE_PEBS].next_tsc >= restarted + ms_to_tsc(&ctx, 10));
    /* Exit can become visible between the first check and task refresh. */
    exit_after_refresh = true;
    restarted = 0;
    ctx.running = true;
    pebs_aggregator_coroutine(NULL);
    assert(!ctx.running && !ctx.sampling_failed && restarted == 0);
    /* Tierce: the window keeps the sampling deadline that the
     * event loop set when the tick started, so the epoch runs start to
     * start, and it makes the PAC step due at once, so the epoch-end marker
     * is applied right after the window. */
    exit_after_refresh = false;
    target_exited = false;
    ctx.mode = PACT_MODE_TIERCE;
    ctx.running = true;
    wl.mlp = 1;
    cpu.owner = 0;
    cpu.count_valid = true; /* the CPU's counting group read this window */
    memcpy((char *)cpu.pebs_mmap + PAGE_SIZE, &record, sizeof(record));
    sample_ring->data_tail = 0;
    sample_ring->data_head = sizeof(record);
    uint64_t tick_deadline = rdtsc() + ms_to_tsc(&ctx, 10);
    ctx.timing[CORO_TYPE_PEBS].next_tsc = tick_deadline;
    ctx.timing[CORO_TYPE_PAC].next_tsc = UINT64_MAX;
    size_t queued = ring_buffer_pac_update_size(ctx.pac_update_ring);
    pebs_aggregator_coroutine(NULL);
    uint64_t after = rdtsc();
    assert(restarted != 0);
    assert(ctx.timing[CORO_TYPE_PEBS].next_tsc == tick_deadline);
    assert(ctx.timing[CORO_TYPE_PAC].next_tsc <= after);
    /* The window pushed its sample and then the epoch-end marker. */
    assert(ring_buffer_pac_update_size(ctx.pac_update_ring) == queued + 2);
    pac_update_t updates[16];
    int n = ring_buffer_pac_update_pop_batch(ctx.pac_update_ring, updates, 16);
    assert(n == (int)queued + 2 && updates[n - 1].workload == PAC_UPDATE_EPOCH_END);
    pebs_aggregator_destroy(agg);
    ring_buffer_pac_update_destroy(ctx.pac_update_ring);
    free(cpu.pebs_mmap);
}
