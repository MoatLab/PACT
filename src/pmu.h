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

/* MEM_TRANS_RETIRED.LOAD_LATENCY (same on SKX and SPR/EMR): retired loads
 * whose latency reaches the floor in config1. */
#define LDLAT_LOADS_EVENT 0x01cdULL
/* Hardware latency floor of the LDLAT event, in cycles. */
#define LDLAT_HW_THRESHOLD 350U
/* MEM_LOADS_AUX (SPR/EMR): an LDLAT event with a data source opens only
 * in a group that holds this event; it is the group leader. */
#define MEM_LOADS_AUX_EVENT 0x8203ULL

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

/* One CHA slice. Only slow-tier TOR traffic is counted: every PEBS sample
 * is a slow-tier sample, so nothing uses fast-tier telemetry. One TOR group
 * (occupancy + cycles) per CHA also keeps TOR occupancy, which counts only
 * on CHA counter 0, from being time-shared between two groups. */
typedef struct cha_pmu_info {
    int cha_id;
    int pmu_type;
    int core_id; /* CPU core associated with this CHA */
    char device_name[64];
    event_group_t group; /* slow-tier TOR occupancy + TOR cycles */
} cha_pmu_info_t;

/* Memory tier of a TOR event or a PEBS sample. */
#define PMU_TIER_SLOW 1

/* Event indices for core/thread counting events */
typedef enum {
    CORE_EVENT_LLC_MISS_SLOW = 0,
    CORE_EVENT_COUNT /* Total number of core counting events */
} core_event_index_t;

/* Events of a Tierce per-CPU counting group. The slow-tier LLC-miss event
 * is always open (slot 0); the others are opened on the CPUs of the CAT
 * foreground workload when the CAT signal needs them. */
typedef enum {
    CPU_EVENT_LLC_MISS_SLOW = 0,
    CPU_EVENT_INSTRUCTIONS,
    CPU_EVENT_CYCLES,
    CPU_EVENT_LLC_MISS_ALL,
    CPU_EVENT_OFFCORE_DD_OUT,
    CPU_EVENT_OFFCORE_DD,
    CPU_EVENT_COUNT
} cpu_event_index_t;

/* Most events in one counting group read. */
#define MAX_GROUP_EVENTS 8
_Static_assert(CPU_EVENT_COUNT <= MAX_GROUP_EVENTS, "per-CPU group fits a group read");
_Static_assert(CORE_EVENT_COUNT <= MAX_GROUP_EVENTS, "per-task group fits a group read");

/* CHA TOR event indices */
typedef enum {
    CHA_TOR_OCCUPANCY = 0,
    CHA_TOR_CYCLES,
    CHA_EVENT_COUNT /* Total number of CHA TOR events per group */
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
    int owner; /* Number of the workload whose CPU set holds this CPU (first match) */

    perf_event_t leader; /* dummy group leader owning the perf mmap */
    /* we don't need ids and values for pebs, no need to use perf_event_t for now */
    int fd_pebs; /*  PEBS file descriptor */

    uint64_t pebs_sampling_period;
    void *pebs_mmap; /*  PEBS buffer */

    /* Tierce: counting group of this CPU (pid = -1), free-running. A
     * failed, malformed or zero-running read removes the CPU from the
     * window: it leaves the LLC-miss sum and its samples are dropped. */
    perf_event_t count_leader;
    perf_event_t count_events[CPU_EVENT_COUNT]; /* open events, in slot order */
    int nr_count_events;
    int count_slot[CPU_EVENT_COUNT];            /* slot of each event, -1 when not open */
    uint64_t count_last_value[CPU_EVENT_COUNT]; /* raw count at the last read */
    uint64_t count_last_enabled, count_last_running;
    bool count_valid;
    uint64_t llc_misses; /* LLC misses of the window, scaled when multiplexed */

    /* Tierce: the CHA paired with this core (index into the context's CHA
     * slices) and the core's slow-tier MLP. A stale reading keeps the last
     * value; a measured zero gives mlp_min. */
    int cha;
    double mlp;
} per_cpu_state_t;

int validate_hardware_access(void);
long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd,
                     unsigned long flags);
int discover_cha_pmus(cha_pmu_info_t *cha_pmus, int *nr_cha, uint64_t cpu_mask);

int setup_dummy_leader_event(perf_event_t *perf_event, pid_t pid, int cpu);
/* SPR/EMR LDLAT: the per-CPU group leader is a MEM_LOADS_AUX counter. */
int setup_mem_loads_aux_leader(perf_event_t *perf_event, int cpu);
/* Open the per-CPU PEBS event: slow-tier LLC-miss loads, or with use_ldlat
 * the LDLAT event (floor LDLAT_HW_THRESHOLD) with weight and data source. */
int setup_pebs_event(per_cpu_state_t *cpu_state, pid_t pid, int cpu, bool use_ldlat);
/* Event configuration tables - populated by pmu_platform_init() */
extern event_config_t core_event_configs[CORE_EVENT_COUNT];
extern event_config_t cpu_event_configs[CPU_EVENT_COUNT];

int setup_counting_event(perf_event_t *perf_event, pid_t pid, int cpu, perf_event_t *leader,
                         uint64_t config, const char *name);
int ioctl_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha, int request);
int ioctl_pmu_core_perf_events(per_cpu_state_t *cpu_states, int nr_target_cpus, int request);
void start_pmu_perf_events(pact_context_t *ctx);
void stop_pmu_perf_events(pact_context_t *ctx);

/* Open the slow-tier TOR group (occupancy + cycles) of every listed CHA.
 * tid_filter on: each CHA counts only its paired core's requests. */
int setup_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int *nr_cha, bool tid_filter);
int setup_tor_events(event_group_t *event_group, int pmu_type, int tier, int core_id);
void read_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha);

int read_pmu_event_group(event_group_t *event_group);
void scale_multiplexed_events(event_group_t *event_group);

/* Window rules shared by every counter reader. */
uint64_t scale_window_count(uint64_t value, uint64_t enabled, uint64_t running);
bool cha_group_fresh(const event_group_t *group);
double mlp_from_counts(uint64_t occupancy, uint64_t cycles);

void read_pmu_counting_events(pact_context_t *ctx);

/* Tierce: open the counting group of one CPU (pid = -1): a dummy leader,
 * the slow-tier LLC-miss event, and the events in extra_events (a mask of
 * 1 << cpu_event_index_t). Returns 0; 1 when an extra event did not open
 * (the extra events of this CPU are then closed); or -1 when the leader or
 * the LLC-miss event did not open (all fds closed). */
int setup_cpu_counting_group(per_cpu_state_t *cs, uint32_t extra_events);
void close_cpu_extra_events(per_cpu_state_t *cs);
void close_cpu_counting_group(per_cpu_state_t *cs);

/* Tierce: workload MLP = the mean of the per-core MLP over the workload's
 * CPUs. A core whose paired CHA reading is fresh takes this window's ratio
 * (mlp_min for zero traffic); any other core keeps its last value. */
void calculate_workload_mlp(pact_context_t *ctx);

/* setup_workload_counting_events() result: every thread of the workload
 * is gone (the task directory is gone, or each listed thread exited). */
#define PACT_WORKLOAD_GONE 1

/* Refresh per-task non-inheriting groups while counters are stopped. Keep
 * existing descriptors, add new tasks, and close exited/reused task groups.
 * A thread that exits during the refresh is dropped, not an error. Returns
 * 0, PACT_WORKLOAD_GONE (no thread is left; the old groups are closed
 * unless the task directory was gone), or -1 with errno for an error. */
struct pact_workload;
int setup_workload_counting_events(struct pact_workload *wl);
void cleanup_workload_counting_events(struct pact_workload *wl);
bool workload_covers_tid(const struct pact_workload *wl, pid_t tid);

#endif
