/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#ifndef PACT_PMU_PLATFORM_H
#define PACT_PMU_PLATFORM_H

#include <stdint.h>

typedef enum {
    PMU_PLATFORM_SKX = 0, /* Skylake-X / Cascade Lake */
    PMU_PLATFORM_EMR,     /* Sapphire Rapids / Emerald Rapids (shared CHA design) */
    PMU_PLATFORM_UNKNOWN
} pmu_platform_id_t;

/*
 * Fully-built perf_event_attr config fields for a single TOR event.
 * Produced by fill_tor_config() -- caller does pure direct assignment,
 * no composition logic (no ORing, no bit positions, no assumptions
 * about which field carries the TID filter).
 */
typedef struct tor_pe_config {
    uint64_t config;
    uint64_t config1;
    uint64_t config2;
} tor_pe_config_t;

typedef struct pmu_platform pmu_platform_t;

struct pmu_platform {
    pmu_platform_id_t id;
    const char *name;

    /* Core events (PEBS + counting) */
    uint64_t event_llc_miss_local;  /* MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM */
    uint64_t event_llc_miss_remote; /* MEM_LOAD_L3_MISS_RETIRED.REMOTE_DRAM */

    /* CHA-to-core mapping */
    int nr_cha_mapping;
    const int *cha_to_core_map;
    const int *core_to_tid;

    /*
     * CHA TOR event config data:
     * tor_base[event_idx][tier]: pre-built pe.config base value
     *   event_idx: CHA_TOR_OCCUPANCY=0, CHA_TOR_CYCLES=1
     *   tier: 0=fast(local), 1=slow(remote)
     */
    uint64_t tor_base[2][2];

    /* config1_base[tier]: pre-built pe.config1 base value (before TID OR). */
    uint64_t config1_base[2];

    uint64_t tid_en_bit; /* OR'd into pe.config when TID filter active */

    /*
     * CHA TOR event config builder.
     * Given (event_idx, tier, core_id), produce fully-built config fields.
     * Reads all data from plat-> fields above -- no hardcoded constants.
     * The caller does pure direct assignment to perf_event_attr.
     */
    void (*fill_tor_config)(const pmu_platform_t *plat, tor_pe_config_t *out, int event_idx,
                            int tier, int core_id);

    /* Tuning constants */
    double mlp_min;
    double mlp_max;
    uint64_t k_constant_dram; /* DRAM access latency in cycles */
    uint64_t k_constant_cxl;  /* CXL access latency in cycles */
};

extern pmu_platform_t g_pmu_platform;

int pmu_platform_init(void); /* call once before CHA setup */

#endif
