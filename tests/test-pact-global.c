/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* PACT mode with two workloads through the actual reader and aggregation:
 * samples map to workloads by PID, every sample gets one global scalar
 * k * sum(LLC misses) / (global MLP * all samples), each update carries its
 * workload number, and PACT pushes no epoch-end entry. The counting-group
 * refresh closes the groups of a workload that exited, once. */
#include "../src/pebs-aggregator.c"
#include <assert.h>

/* PAC update ring entry: the 64-bit PAC word, then the workload number. */
_Static_assert(sizeof(pac_update_t) == 16, "16-byte ring entry");
_Static_assert(offsetof(pac_update_t, encoded) == 0, "PAC word first");
_Static_assert(offsetof(pac_update_t, workload) == 8, "workload number after the PAC word");

bool workload_covers_tid(const pact_workload_t *wl, pid_t tid)
{
    (void)wl;
    return tid != 99; /* TID 99 has no counting group this window */
}

/* Counting-group stubs: count the refreshes and closes of each workload. */
static int setups[2], cleanups[2];

int setup_workload_counting_events(pact_workload_t *wl)
{
    setups[wl->index]++;
    return 0;
}

void cleanup_workload_counting_events(pact_workload_t *wl)
{
    cleanups[wl->index]++;
    wl->threads = NULL;
    wl->nr_threads = 0;
}

bool pact_workload_exited(pact_context_t *ctx, pact_workload_t *wl)
{
    (void)ctx;
    return wl->exited;
}

void pact_mark_workload_exited(pact_workload_t *wl)
{
    wl->exited = true;
}

static uint64_t ring_pos;

static void add_sample(per_cpu_state_t *cpu, uint32_t pid, uint32_t tid, uint64_t addr)
{
    struct {
        struct perf_event_header header;
        uint32_t pid, tid;
        uint64_t addr;
    } r = {{.type = PERF_RECORD_SAMPLE, .size = sizeof(r)}, pid, tid, addr};
    size_t bytes = PERF_BUFFER_PAGES * PAGE_SIZE;
    char *data = (char *)cpu->pebs_mmap + PAGE_SIZE;
    for (size_t i = 0; i < sizeof(r); i++) {
        data[(ring_pos + i) % bytes] = ((const char *)&r)[i];
    }
    ring_pos += sizeof(r);
    ((struct perf_event_mmap_page *)cpu->pebs_mmap)->data_head = ring_pos;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    per_cpu_state_t cpu = {.fd_pebs = 1};
    cpu.pebs_mmap = calloc(1, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
    assert(cpu.pebs_mmap);
    pact_workload_t wl[2] = {{.index = 0, .target_pid = 42}, {.index = 1, .target_pid = 43}};
    wl[0].stats.llc_misses_slow = 100;
    wl[1].stats.llc_misses_slow = 300;
    pact_context_t ctx = {.mode = PACT_MODE_PACT,
                          .workloads = wl,
                          .nr_workloads = 2,
                          .global_mlp = 2.0,
                          .k_constant_cxl = 771,
                          .pebs_sampling_period = 400};
    ctx.pac_update_ring = ring_buffer_pac_update_create(16);
    pebs_aggregator_t *agg = pebs_aggregator_create(&cpu, 1, 1);
    agg->pact_ctx = &ctx;

    add_sample(&cpu, 42, 42, 0x1000);
    add_sample(&cpu, 43, 43, 0x2000);
    add_sample(&cpu, 43, 44, 0x3000);
    add_sample(&cpu, 42, 42, 0x4000);
    add_sample(&cpu, 7, 7, 0x5000);   /* another process: ignored */
    add_sample(&cpu, 42, 99, 0x6000); /* uncovered thread: dropped */
    /* S = 771 * (100 + 300) / (2 * 4) = 38550 for every sample. */
    assert(pebs_aggregate_events(agg, &ctx) == 4);
    assert(agg->samples[0] == 2 && agg->samples[1] == 2 && agg->total_samples == 4);
    assert(wl[0].stats.pebs_uncovered == 1 && wl[1].stats.pebs_uncovered == 0);
    uint32_t expect_wl[4] = {0, 1, 1, 0};
    uint64_t expect_page[4] = {0x1000, 0x2000, 0x3000, 0x4000};
    for (int i = 0; i < 4; i++) {
        pac_update_t u;
        assert(ring_buffer_pac_update_pop(ctx.pac_update_ring, &u));
        assert(u.workload == expect_wl[i] && PEBS_DECODE_PAGE(u.encoded) == expect_page[i]);
        assert(PEBS_DECODE_PAC(u.encoded) == 38550 && PEBS_DECODE_TIER(u.encoded) == 1);
    }
    assert(ring_buffer_pac_update_size(ctx.pac_update_ring) == 0); /* no epoch end */

    /* The 18-bit PAC field keeps its maximum next to a 57-bit page address,
     * and the workload number travels in its own field. */
    uint64_t top_page = (((uint64_t)1 << 57) - 1) & PAGE_MASK;
    pac_update_t big = {.encoded = PEBS_ENCODE_PAC(PAC_VALUE_MAX, top_page, 1),
                        .workload = MAX_WORKLOADS - 1};
    assert(ring_buffer_pac_update_push(ctx.pac_update_ring, big));
    pac_update_t back;
    assert(ring_buffer_pac_update_pop(ctx.pac_update_ring, &back));
    assert(PEBS_DECODE_PAC(back.encoded) == PAC_VALUE_MAX);
    assert(PEBS_DECODE_PAGE(back.encoded) == top_page && PEBS_DECODE_TIER(back.encoded) == 1);
    assert(back.workload == MAX_WORKLOADS - 1);

    /* The liveness poll marks workload 0 as exited before the refresh. The
     * refresh then closes its groups once and refreshes workload 1 only. */
    thread_perf_t group[2] = {{.tid = 42}, {.tid = 43}};
    wl[0].threads = &group[0];
    wl[0].nr_threads = 1;
    wl[1].threads = &group[1];
    wl[1].nr_threads = 1;
    atomic_store(&wl[0].exited, true);
    assert(refresh_counting_groups(&ctx) == 0);
    assert(cleanups[0] == 1 && setups[0] == 0 && wl[0].threads == NULL);
    assert(setups[1] == 1 && cleanups[1] == 0);
    assert(refresh_counting_groups(&ctx) == 0);
    assert(cleanups[0] == 1 && setups[0] == 0 && setups[1] == 2);

    ring_buffer_pac_update_destroy(ctx.pac_update_ring);
    pebs_aggregator_destroy(agg);
    free(cpu.pebs_mmap);
    return 0;
}
