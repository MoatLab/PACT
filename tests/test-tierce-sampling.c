/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* Tierce sampling through the actual reader and aggregation: CPU ownership
 * and coverage, the per-workload stall scalar, the epoch-end entry, and the
 * LDLAT filter, gate, latency-weighted S_p and k_slow update. */
#include "../src/pebs-aggregator.c"
#include <assert.h>
#include <math.h>

bool workload_covers_tid(const pact_workload_t *wl, pid_t tid)
{
    (void)wl;
    (void)tid;
    assert(false); /* PACT-only rule */
    return false;
}

static uint64_t ring_pos;

/* Append one record at the producer position of the CPU's perf ring. */
static void append(per_cpu_state_t *cpu, const void *record, size_t size)
{
    size_t bytes = PERF_BUFFER_PAGES * PAGE_SIZE;
    char *data = (char *)cpu->pebs_mmap + PAGE_SIZE;
    for (size_t i = 0; i < size; i++) {
        data[(ring_pos + i) % bytes] = ((const char *)record)[i];
    }
    ring_pos += size;
    ((struct perf_event_mmap_page *)cpu->pebs_mmap)->data_head = ring_pos;
}

typedef struct {
    struct perf_event_header header;
    uint32_t pid, tid;
    uint64_t addr;
} plain_record_t;

typedef struct {
    struct perf_event_header header;
    uint32_t pid, tid;
    uint64_t addr, weight, data_src;
} ldlat_record_t;

static void add_plain(per_cpu_state_t *cpu, uint32_t pid, uint64_t addr)
{
    plain_record_t r = {{.type = PERF_RECORD_SAMPLE, .size = sizeof(r)}, pid, pid, addr};
    append(cpu, &r, sizeof(r));
}

static void add_ldlat(per_cpu_state_t *cpu, uint32_t pid, uint64_t addr, uint64_t latency,
                      uint64_t data_src)
{
    ldlat_record_t r = {
        {.type = PERF_RECORD_SAMPLE, .size = sizeof(r)}, pid, pid, addr, latency, data_src};
    append(cpu, &r, sizeof(r));
}

static pac_update_t pop(pact_context_t *ctx)
{
    pac_update_t update;
    assert(ring_buffer_pac_update_pop(ctx->pac_update_ring, &update));
    return update;
}

/* PAC value of the next entry (PEBS_DECODE_PAC reads its argument twice). */
static uint32_t pop_pac(pact_context_t *ctx)
{
    pac_update_t update = pop(ctx);
    assert(update.workload != PAC_UPDATE_EPOCH_END);
    return PEBS_DECODE_PAC(update.encoded);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    per_cpu_state_t cpu = {.fd_pebs = 1, .owner = 0, .count_valid = true};
    cpu.pebs_mmap = calloc(1, (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
    assert(cpu.pebs_mmap);
    pact_workload_t wl = {.target_pid = 42, .mlp = 2.0, .k_slow = 771.0};
    pact_context_t ctx = {.mode = PACT_MODE_TIERCE,
                          .workloads = &wl,
                          .nr_workloads = 1,
                          .k_constant_cxl = 771,
                          .pebs_sampling_period = 100};
    pebs_aggregator_t *agg = pebs_aggregator_create(&cpu, 1, 1);
    agg->pact_ctx = &ctx;
    staged_sample_t events[8];

    /* Ownership: another process on the CPU is ignored; a CPU whose
     * counting group did not cover the window drops its samples. */
    add_plain(&cpu, 42, 0x1000);
    add_plain(&cpu, 7, 0x2000);
    assert(read_cpu_pebs_events(agg, &cpu, events, 8) == 1);
    assert(events[0].workload == 0 && events[0].page == 0x1000);
    assert(wl.stats.pebs_uncovered == 0);
    cpu.count_valid = false;
    add_plain(&cpu, 42, 0x1000);
    assert(read_cpu_pebs_events(agg, &cpu, events, 8) == 0);
    assert(wl.stats.pebs_uncovered == 1);
    cpu.count_valid = true;
    reset_per_cycle_counters(agg);

    /* S_w = k * LLC_w / (MLP_w * N_w * P) = 771 * 20000 / (2 * 3 * 100) = 25700.
     * The ring holds 3 entries: two samples fit, one slot stays for the
     * epoch-end entry, and the third sample is a drop. */
    ctx.pac_update_ring = ring_buffer_pac_update_create(4);
    wl.stats.llc_misses_slow = 20000;
    for (int i = 0; i < 3; i++) {
        add_plain(&cpu, 42, 0x10000 + (uint64_t)i * PAGE_SIZE);
    }
    assert(pebs_aggregate_events(agg, &ctx) == 2);
    assert(ctx.stats.pebs_update_drops == 1);
    for (int i = 0; i < 2; i++) {
        assert(pop_pac(&ctx) == 25700);
    }
    assert(pop(&ctx).workload == PAC_UPDATE_EPOCH_END);
    /* No sample, no epoch-end entry. */
    reset_per_cycle_counters(agg);
    assert(pebs_aggregate_events(agg, &ctx) == 0);
    assert(ring_buffer_pac_update_size(ctx.pac_update_ring) == 0);
    ring_buffer_pac_update_destroy(ctx.pac_update_ring);

    /* LDLAT with gate 700, period 5. A local load is filtered out. A remote
     * 400-cycle load feeds k_slow but is below the gate. Loads of 800 and
     * 1000 cycles (legacy and newer remote encodings) are kept. */
    ctx.use_ldlat = true;
    ctx.ldlat_gate = 700;
    ctx.pebs_sampling_period = 5;
    ctx.pac_update_ring = ring_buffer_pac_update_create(8);
    wl.stats.llc_misses_slow = 1000;
    uint64_t local = (uint64_t)PERF_MEM_LVL_LOC_RAM << PERF_MEM_LVL_SHIFT;
    uint64_t remote_legacy = (uint64_t)(PERF_MEM_LVL_REM_RAM1 | PERF_MEM_LVL_HIT)
                             << PERF_MEM_LVL_SHIFT;
    uint64_t remote_new = ((uint64_t)PERF_MEM_LVLNUM_RAM << PERF_MEM_LVLNUM_SHIFT) |
                          ((uint64_t)1 << PERF_MEM_REMOTE_SHIFT);
    reset_per_cycle_counters(agg);
    add_ldlat(&cpu, 42, 0x30000, 900, local);
    add_ldlat(&cpu, 42, 0x31000, 400, remote_legacy);
    add_ldlat(&cpu, 42, 0x32000, 800, remote_legacy);
    add_ldlat(&cpu, 42, 0x33000, 1000, remote_new);
    /* S_w = 771 * 1000 / (2 * 2 * 5) = 38550 = 50 * k_slow, so the S_p values
     * are 50 * latency with k_slow = 771 from before the update. */
    assert(pebs_aggregate_events(agg, &ctx) == 2);
    assert(wl.ldlat_filtered == 1 && wl.ldlat_gated == 1);
    assert(pop_pac(&ctx) == 40000);
    assert(pop_pac(&ctx) == 50000);
    assert(pop(&ctx).workload == PAC_UPDATE_EPOCH_END);
    /* k_slow input: every remote load before the gate (400, 800, 1000). */
    double mean = (400.0 + 800.0 + 1000.0) / 3.0;
    assert(fabs(wl.k_slow - (0.3 * mean + 0.7 * 771.0)) < 1e-9);

    /* A window with only filtered loads keeps k_slow. */
    double before = wl.k_slow;
    reset_per_cycle_counters(agg);
    add_ldlat(&cpu, 42, 0x34000, 900, local);
    assert(pebs_aggregate_events(agg, &ctx) == 0);
    assert(wl.k_slow == before && wl.ldlat_filtered == 2);

    /* Gate 350 (the floor) drops nothing more. var2_w decides even when
     * var1_dw is below the floor; at its maximum S_p saturates. */
    ctx.ldlat_gate = LDLAT_HW_THRESHOLD;
    reset_per_cycle_counters(agg);
    add_ldlat(&cpu, 42, 0x35000, 400, remote_new);
    add_ldlat(&cpu, 42, 0x36000, ((uint64_t)0xFFFF << 32) | 300, remote_new);
    assert(pebs_aggregate_events(agg, &ctx) == 2);
    assert(wl.ldlat_gated == 1);
    pop(&ctx);
    assert(pop_pac(&ctx) == PAC_VALUE_MAX);
    assert(pop(&ctx).workload == PAC_UPDATE_EPOCH_END);

    /* LDLAT-Dyn in TIGHT mode (gate 700): a 400-cycle remote load feeds
     * k_slow and is then gated; an 800-cycle load is admitted. */
    ctx.ldlat_dyn.enabled = true;
    ctx.ldlat_dyn.gate = 700;
    wl.k_slow = 600.0;
    reset_per_cycle_counters(agg);
    add_ldlat(&cpu, 42, 0x37000, 400, remote_new);
    add_ldlat(&cpu, 42, 0x38000, 800, remote_new);
    assert(pebs_aggregate_events(agg, &ctx) == 1);
    assert(wl.ldlat_gated == 2 && ctx.ldlat_dyn.admitted == 1);
    assert(fabs(wl.k_slow - (0.3 * 600.0 + 0.7 * 600.0)) < 1e-9);
    pop(&ctx);
    assert(pop(&ctx).workload == PAC_UPDATE_EPOCH_END);

    ring_buffer_pac_update_destroy(ctx.pac_update_ring);
    pebs_aggregator_destroy(agg);
    free(cpu.pebs_mmap);
    return 0;
}
