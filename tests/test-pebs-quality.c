/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include "../src/pebs-aggregator.c"
#include <assert.h>
bool workload_covers_tid(const pact_workload_t *wl, pid_t tid)
{
    (void)wl;
    return tid == 42;
}
static void load_record(per_cpu_state_t *cpu, uint64_t start, const void *record, size_t size)
{
    size_t bytes = PERF_BUFFER_PAGES * PAGE_SIZE;
    char *data = (char *)cpu->pebs_mmap + PAGE_SIZE;
    for (size_t i = 0; i < size; i++) {
        data[(start + i) % bytes] = ((const char *)record)[i];
    }
    struct perf_event_mmap_page *meta = cpu->pebs_mmap;
    meta->data_tail = start;
    meta->data_head = start + size;
}
int main(void)
{
    per_cpu_state_t cpu = {.fd_pebs = 1};
    cpu.pebs_mmap = calloc(1, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
    assert(cpu.pebs_mmap);
    struct perf_event_mmap_page *meta = cpu.pebs_mmap;
    pact_workload_t wl = {.target_pid = 42, .counters_valid = true};
    pact_context_t ctx = {.workload = &wl};
    pebs_aggregator_t *agg = pebs_aggregator_create(&cpu, 1, 1);
    agg->pact_ctx = &ctx;
    struct perf_event_header header = {.type = PERF_RECORD_SAMPLE, .size = 32};
    memcpy((char *)cpu.pebs_mmap + PAGE_SIZE, &header, sizeof(header));
    meta->data_head = sizeof(header);
    uint64_t events[2];
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(meta->data_tail == meta->data_head);
    assert(wl.stats.pebs_malformed == 1);
    /* A short header and an overrun must not read or retain unpublished bytes. */
    meta->data_tail = 0;
    meta->data_head = 4;
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_malformed == 2 && meta->data_tail == meta->data_head);
    meta->data_tail = 0;
    meta->data_head = PERF_BUFFER_PAGES * PAGE_SIZE + 8;
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_overruns == 1 && meta->data_tail == meta->data_head);
    struct {
        struct perf_event_header header;
        uint32_t pid, tid;
        uint64_t time, addr;
    } sample = {{.type = PERF_RECORD_SAMPLE, .size = 32}, 42, 42, 123456, 8192};
    uint64_t starts[] = {0, PERF_BUFFER_PAGES * PAGE_SIZE - 4, UINT64_MAX - 15};
    for (size_t i = 0; i < 3; i++) {
        load_record(&cpu, starts[i], &sample, sizeof(sample));
        assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 1);
        assert(events[0] == PEBS_ENCODE_ADDR_TIER(sample.addr, 1));
        assert(meta->data_tail == meta->data_head);
    }
    sample.tid = 43;
    load_record(&cpu, 0, &sample, sizeof(sample));
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_uncovered == 1);
    sample.pid = 99;
    load_record(&cpu, 0, &sample, sizeof(sample));
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_uncovered == 1);
    sample.pid = sample.tid = 42;
    wl.counters_valid = false;
    load_record(&cpu, 0, &sample, sizeof(sample));
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_uncovered == 2);
    wl.counters_valid = true;
    struct {
        struct perf_event_header header;
        uint64_t id, lost;
    } loss = {{.type = PERF_RECORD_LOST, .size = 24}, 99, 13};
    load_record(&cpu, PERF_BUFFER_PAGES * PAGE_SIZE - 8, &loss, sizeof(loss));
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_lost_samples == 13 && wl.stats.pebs_lost_records == 1);
    struct {
        struct perf_event_header header;
        uint64_t lost;
    } lost_samples = {{.type = PERF_RECORD_LOST_SAMPLES, .size = 16}, 7};
    load_record(&cpu, 0, &lost_samples, sizeof(lost_samples));
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_lost_samples == 20 && wl.stats.pebs_lost_records == 2);
    struct {
        struct perf_event_header header;
        uint64_t time, id, stream_id;
    } throttle = {{.type = PERF_RECORD_THROTTLE, .size = 32}, 1, 2, 3};
    load_record(&cpu, 0, &throttle, sizeof(throttle));
    assert(read_cpu_pebs_events(agg, &cpu, events, 2) == 0);
    assert(wl.stats.pebs_throttles == 1);
    ctx.pac_update_ring = ring_buffer_uint64_create(2);
    assert(ring_buffer_uint64_push(ctx.pac_update_ring, 0));
    /* Exercise both publication drops and staging drops through real aggregation. */
    for (int staging = 0; staging < 2; staging++) {
        load_record(&cpu, 0, &sample, sizeof(sample));
        load_record(&cpu, sizeof(sample), &sample, sizeof(sample));
        meta->data_tail = 0;
        if (staging) {
            agg->cycle_buf_cap = 1;
        }
        assert(pebs_aggregate_events(agg, &ctx) == 0);
        assert(wl.stats.pebs_update_drops == (uint64_t)(staging + 1) * 2);
        assert(meta->data_tail == meta->data_head);
        reset_per_cycle_counters(agg);
        assert(wl.stats.pebs_lost_samples == 20 && wl.stats.pebs_throttles == 1);
    }
    ring_buffer_uint64_destroy(ctx.pac_update_ring);
    pebs_aggregator_destroy(agg);
    free(cpu.pebs_mmap);
}
