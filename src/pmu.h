/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#ifndef PACT_PMU_H
#define PACT_PMU_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <linux/perf_event.h>

#include "minicoro.h"
#include "ring-buffer.h"
#include "pmu-platform.h"

typedef struct pact_context pact_context_t;

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
/* page mask assuming 57-bit virtual address space */
#define VADDR_MASK ((1UL << 57) - 1)
#define PAGE_MASK ((~(PAGE_SIZE - 1)) & VADDR_MASK)
#define PERF_BUFFER_PAGES 512

#define MAX_CHAS 64 /*  Maximum number of CHAs we expect to find */

typedef struct event_group {
    int counters_used; /*  number of counters used in each group */
    int fds[4];
    uint64_t ids[4];    /*  IDs for read_format */
    uint64_t values[4]; /*  values read from the counters */
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t last_time_enabled;
    uint64_t last_time_running;
    bool read_valid;
    bool needs_resync; /* A failed read loses the scheduling-time window boundary. */
} event_group_t;

typedef struct cha_pmu_info {
    int cha_id;
    int pmu_type;
    int core_id; /* CPU core associated with this CHA */
    char device_name[64];
    /* we have two groups for now */
    event_group_t group_fast;
    event_group_t group_slow;
} cha_pmu_info_t;

/* Event indices for core/thread counting events */
typedef enum {
    CORE_EVENT_LLC_MISS_FAST = 0,
    CORE_EVENT_LLC_MISS_SLOW,
    CORE_EVENT_COUNT /* Total number of core counting events */
} core_event_index_t;

/* CHA TOR event indices */
typedef enum {
    CHA_TOR_OCCUPANCY = 0,
    CHA_TOR_CYCLES,
    CHA_EVENT_COUNT /* Total number of CHA TOR events per tier */
} cha_event_index_t;

/* Event configuration for mapping enum to hardware config */
typedef struct event_config {
    uint64_t config;
    const char *name;
} event_config_t;

/* Structure for reading grouped PMU events */
typedef struct read_format {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    struct {
        uint64_t value;
        uint64_t id;
    } values[];
} read_format_t;

typedef struct perf_event {
    int fd;
    uint64_t id;
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
} perf_event_t;

/* Per-task counter ownership, refreshed between sampling windows. */
typedef struct thread_perf {
    pid_t tid;
    uint64_t start_time; /* /proc start ticks disambiguate reused TIDs. */
    perf_event_t leader;
    perf_event_t events[CORE_EVENT_COUNT];
    uint64_t last_enabled, last_running;
    bool valid;
} thread_perf_t;

/* Per-CPU sampling state. */
typedef struct per_cpu_state {
    int cpu_id;

    perf_event_t leader; /* dummy group leader owning the perf mmap */
    /* we don't need ids and values for pebs, no need to use perf_event_t for now */
    int fd_pebs; /*  PEBS file descriptor */

    uint64_t pebs_sampling_period;
    void *pebs_mmap; /*  PEBS buffer */
} per_cpu_state_t;

int validate_hardware_access(void);
long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd,
                     unsigned long flags);
int discover_cha_pmus(cha_pmu_info_t *cha_pmus, int *nr_cha, uint64_t cpu_mask);

int setup_dummy_leader_event(perf_event_t *perf_event, pid_t pid, int cpu);
int setup_pebs_event(per_cpu_state_t *cpu_state, pid_t pid, int cpu);
/* Event configuration table - populated by pmu_platform_init() */
extern event_config_t core_event_configs[CORE_EVENT_COUNT];

int setup_counting_event(perf_event_t *perf_event, pid_t pid, int cpu, perf_event_t *leader,
                         uint64_t config, const char *name);
void ioctl_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha, int request);
void ioctl_pmu_core_perf_events(per_cpu_state_t *cpu_states, int nr_target_cpus, int request);
void start_pmu_perf_events(pact_context_t *ctx);
void stop_pmu_perf_events(pact_context_t *ctx);

int setup_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int *nr_cha); /*  tor_occ + tor_cyc */
int setup_tor_events(event_group_t *event_group, int pmu_type, int tier, int core_id);
void read_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha);

int read_pmu_event_group(event_group_t *event_group);
void scale_multiplexed_events(event_group_t *event_group);

void read_pmu_counting_events(pact_context_t *ctx);

/* Refresh per-task non-inheriting groups while counters are stopped. Keep
 * existing descriptors, add new tasks, and close exited/reused task groups. */
struct pact_workload;
int setup_workload_counting_events(struct pact_workload *wl);
void cleanup_workload_counting_events(struct pact_workload *wl);
bool workload_covers_tid(const struct pact_workload *wl, pid_t tid);

#endif
