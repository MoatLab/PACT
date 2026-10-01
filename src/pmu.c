/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include <time.h>

#include "constants.h"
#include "pact.h"
#include "pmu.h"
#include "perf.h"
#include "error.h"

/* Event configuration table for core/thread counting events.
 * Populated at runtime by pmu_platform_init(). */
event_config_t core_event_configs[CORE_EVENT_COUNT];

int validate_hardware_access(void)
{
    /* Initialize NUMA */
    if (numa_available() < 0) {
        log_warning("validate_hardware_access", "NUMA not available, migrations will be simulated");
        return 0;
    }

    /* Check if we have the necessary permissions for hardware counters */
    if (geteuid() != 0) {
        log_warning("validate_hardware_access",
                    "Running without root privileges - hardware counters may not be accessible");
        return 0; /* Not fatal, but degraded functionality */
    }

    /* Check if /proc/sys/kernel/perf_event_paranoid allows access */
    FILE *paranoid_file = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (paranoid_file) {
        int paranoid_level;
        if (fscanf(paranoid_file, "%d", &paranoid_level) == 1) {
            if (paranoid_level > 1) {
                log_warning("validate_hardware_access",
                            "perf_event_paranoid level is high - some counters may be restricted");
            }
        }
        fclose(paranoid_file);
    }

    return 1; /* Success */
}

long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd,
                     unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/* Function to discover CHA PMUs for target CPUs only */
typedef struct {
    int cha_id;
    int pmu_type;
    char device_name[64];
} cha_discovery_t;

/* Zero-init every cha_pmu_info_t slot to "unused" sentinels (-1 IDs, -1 fds). */
static void cha_pmus_clear_slots(cha_pmu_info_t *cha_pmus)
{
    for (int i = 0; i < MAX_CHAS; i++) {
        cha_pmus[i].cha_id = -1;
        cha_pmus[i].pmu_type = -1;
        cha_pmus[i].core_id = -1;
        cha_pmus[i].device_name[0] = '\0';
        for (int j = 0; j < 4; j++) {
            cha_pmus[i].group_fast.fds[j] = -1;
            cha_pmus[i].group_slow.fds[j] = -1;
            cha_pmus[i].group_fast.ids[j] = -1;
            cha_pmus[i].group_slow.ids[j] = -1;
        }
    }
}

/* Read PMU type for a single uncore_cha_* device and record it into the
 * discovered[] buffer. Returns true on success. */
static bool record_cha_device(const char *dev_name, int cha_id, cha_discovery_t *out)
{
    char path[256];
    int ret = snprintf(path, sizeof(path), "/sys/devices/%s/type", dev_name);
    if (ret >= (int)sizeof(path)) {
        log_warning("detect_cha_pmus", "Path too long for device %s, skipping", dev_name);
        return false;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        exit(EXIT_FAILURE); /* preserved: legacy behavior on /sys read failure */
    }
    int pmu_type;
    bool ok = (fscanf(fp, "%d", &pmu_type) == 1);
    fclose(fp);
    if (!ok) {
        return false;
    }
    out->cha_id = cha_id;
    out->pmu_type = pmu_type;
    strncpy(out->device_name, dev_name, sizeof(out->device_name) - 1);
    out->device_name[sizeof(out->device_name) - 1] = '\0';
    return true;
}

/* Discover all uncore_cha_* devices under /sys/devices. Fills the discovered[]
 * buffer and returns count. */
static int discover_all_cha_devices(cha_discovery_t *discovered)
{
    DIR *dir = opendir("/sys/devices");
    if (!dir) {
        perror("Failed to open /sys/devices");
        return -1;
    }
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "uncore_cha_", 11) != 0) {
            continue;
        }
        int cha_id = atoi(entry->d_name + 11);
        if (cha_id < 0 || cha_id >= MAX_CHAS) {
            log_warning("detect_cha_pmus", "CHA ID %d is out of bounds (0-%d), skipping", cha_id,
                        MAX_CHAS - 1);
            exit(EXIT_FAILURE);
        }
        if (record_cha_device(entry->d_name, cha_id, &discovered[n])) {
            n++;
        }
    }
    closedir(dir);
    return n;
}

/* Find the smallest pmu_type — used as the origin for CHA-offset → core
 * mapping in g_pmu_platform.cha_to_core_map. */
static int find_base_pmu_type(const cha_discovery_t *discovered, int n)
{
    int base = INT_MAX;
    for (int i = 0; i < n; i++) {
        if (discovered[i].pmu_type < base) {
            base = discovered[i].pmu_type;
        }
    }
    return base;
}

/* Filter the discovered CHAs by cpu_mask and populate cha_pmus[] for the
 * active set. Returns count of active CHAs and writes count of skipped. */
static int filter_active_chas(const cha_discovery_t *discovered, int n_discovered,
                              int base_pmu_type, uint64_t cpu_mask, cha_pmu_info_t *cha_pmus,
                              int *out_skipped)
{
    int active = 0, skipped = 0;
    for (int i = 0; i < n_discovered; i++) {
        int cha_id = discovered[i].cha_id;
        int pmu_type = discovered[i].pmu_type;
        int cha_offset = pmu_type - base_pmu_type;

        if (cha_offset < 0 || cha_offset >= g_pmu_platform.nr_cha_mapping) {
            log_debug("detect_cha_pmus",
                      "CHA %d: offset %d out of range (only %d mapped), skipping", cha_id,
                      cha_offset, g_pmu_platform.nr_cha_mapping);
            skipped++;
            continue;
        }
        int core_id = g_pmu_platform.cha_to_core_map[cha_offset];
        if (core_id >= 0 && cpu_mask != 0 && !(cpu_mask & (1ULL << core_id))) {
            log_debug("detect_cha_pmus", "Skipping CHA %d (core %d not in target CPU mask 0x%lx)",
                      cha_id, core_id, cpu_mask);
            skipped++;
            continue;
        }

        cha_pmus[active].cha_id = cha_id;
        cha_pmus[active].pmu_type = pmu_type;
        cha_pmus[active].core_id = core_id;
        strncpy(cha_pmus[active].device_name, discovered[i].device_name,
                sizeof(cha_pmus[active].device_name) - 1);
        cha_pmus[active].device_name[sizeof(cha_pmus[active].device_name) - 1] = '\0';
        log_info("detect_cha_pmus", "Added CHA %d: %s (PMU type %d, core %d) at index %d", cha_id,
                 discovered[i].device_name, pmu_type, core_id, active);
        active++;
    }
    *out_skipped = skipped;
    return active;
}

int discover_cha_pmus(cha_pmu_info_t *cha_pmus, int *nr_cha, uint64_t cpu_mask)
{
    cha_pmus_clear_slots(cha_pmus);

    cha_discovery_t discovered[MAX_CHAS];
    int n_discovered = discover_all_cha_devices(discovered);
    if (n_discovered < 0) {
        return -1;
    }
    if (n_discovered == 0) {
        log_info("detect_cha_pmus", "No CHA PMUs discovered");
        *nr_cha = 0;
        return 0;
    }
    log_info("detect_cha_pmus", "Total CHA PMUs discovered: %d", n_discovered);

    int base = find_base_pmu_type(discovered, n_discovered);
    int skipped = 0;
    int active = filter_active_chas(discovered, n_discovered, base, cpu_mask, cha_pmus, &skipped);
    *nr_cha = active;
    if (skipped > 0) {
        log_info("detect_cha_pmus", "Active CHA PMUs: %d (skipped %d CHAs not in target CPU mask)",
                 active, skipped);
    } else {
        log_info("detect_cha_pmus", "Active CHA PMUs: %d", active);
    }
    return active;
}

int setup_tor_events(event_group_t *event_group, int pmu_type, int tier, int core_id)
{
    const pmu_platform_t *plat = &g_pmu_platform;
    static const char *event_names[CHA_EVENT_COUNT] = {"TOR_OCCUPANCY", "TOR_CYCLES"};
    int leader_fd = -1;

    for (int i = 0; i < CHA_EVENT_COUNT; i++) {
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.size = sizeof(pe);
        pe.type = pmu_type;
        pe.sample_type = PERF_SAMPLE_IDENTIFIER;
        pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                         PERF_FORMAT_GROUP | PERF_FORMAT_ID;

        /* All config composition delegated to the platform */
        tor_pe_config_t cfg;
        plat->fill_tor_config(plat, &cfg, i, tier, core_id);
        pe.config = cfg.config;
        pe.config1 = cfg.config1;
        pe.config2 = cfg.config2;

        pe.disabled = (i == CHA_TOR_OCCUPANCY) ? 1 : 0;
        pe.exclude_kernel = 0;
        pe.exclude_hv = 0;
        pe.exclude_idle = 0;
        pe.inherit = 1;

        int group_fd = (i == CHA_TOR_OCCUPANCY) ? -1 : leader_fd;
        int fd = perf_event_open(&pe, -1, 0, group_fd, 0);

        if (fd == -1) {
            log_warning("setup_tor_events", "Error opening %s event (tier %d): %s", event_names[i],
                        tier, strerror(errno));
            return -1;
        }

        ioctl(fd, PERF_EVENT_IOC_ID, &event_group->ids[i]);
        event_group->fds[i] = fd;
        event_group->counters_used++;

        if (i == CHA_TOR_OCCUPANCY) {
            leader_fd = fd;
        }
    }

    return 0;
}

/* Setup perf events for active CHAs (already filtered by discover_cha_pmus) */
static void close_event_group_fds(event_group_t *g)
{
    for (int j = 0; j < g->counters_used; j++) {
        if (g->fds[j] != -1) {
            close(g->fds[j]);
            g->fds[j] = -1;
        }
    }
}

/* Cleanup partially-opened TOR event fds for one CHA after setup failure.
 * Fixes a prior bug where both loops indexed group_slow.fds, leaking the
 * group_fast fds when slow-group setup failed after fast-group success. */
static void cleanup_partial_cha_setup(cha_pmu_info_t *cha)
{
    close_event_group_fds(&cha->group_fast);
    close_event_group_fds(&cha->group_slow);
}

/* Open the fast (tier 0) and slow (tier 1) TOR event groups for one CHA.
 * On any group failure, both groups' fds are closed via cleanup helper.
 * Returns the number of events successfully opened (0 on failure). */
static int setup_one_cha_tor_groups(cha_pmu_info_t *cha)
{
    int fast_ok = setup_tor_events(&cha->group_fast, cha->pmu_type, 0, cha->core_id) == 0;
    int slow_ok =
        fast_ok && setup_tor_events(&cha->group_slow, cha->pmu_type, 1, cha->core_id) == 0;
    if (slow_ok) {
        return cha->group_fast.counters_used + cha->group_slow.counters_used;
    }
    fprintf(stderr, "  Failed to setup TOR events for CHA %d, skipping\n", cha->cha_id);
    cleanup_partial_cha_setup(cha);
    return 0;
}

int setup_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int *nr_cha)
{
    int total_events = 0;
    int active_cha_count = 0;

    for (int i = 0; i < *nr_cha; i++) {
        log_info("setup_uncore_events", "Setting up CHA %d (%s, PMU type %d, core %d)",
                 cha_pmus[i].cha_id, cha_pmus[i].device_name, cha_pmus[i].pmu_type,
                 cha_pmus[i].core_id);
        int n = setup_one_cha_tor_groups(&cha_pmus[i]);
        if (n > 0) {
            active_cha_count++;
            total_events += n;
        }
    }
    log_info("setup_uncore_events", "Successfully opened %d individual events across %d CHAs",
             total_events, active_cha_count);
    return total_events;
}

/* Every group operation must succeed before its measurements can be used.
 * Keep trying other groups on failure so DISABLE performs best-effort cleanup. */
static int control_pmu_group(int fd, int request)
{
    if (ioctl(fd, request, PERF_IOC_FLAG_GROUP) == 0) {
        return 0;
    }
    log_error("pmu_control", "Failed request 0x%x on fd %d: %s", request, fd, strerror(errno));
    return -1;
}

int ioctl_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha, int request)
{
    int result = 0;
    for (int i = 0; i < nr_cha; i++) {
        if (control_pmu_group(cha_pmus[i].group_fast.fds[0], request) < 0) {
            result = -1;
        }
        if (control_pmu_group(cha_pmus[i].group_slow.fds[0], request) < 0) {
            result = -1;
        }
    }
    return result;
}

int ioctl_pmu_core_perf_events(per_cpu_state_t *cpu_states, int nr_target_cpus, int request)
{
    int result = 0;
    for (int i = 0; i < nr_target_cpus; i++) {
        /* Offline CPUs have no group. Errors on existing groups are fatal. */
        int fd = cpu_states[i].leader.fd;
        if (fd >= 0 && control_pmu_group(fd, request) < 0) {
            result = -1;
        }
    }
    return result;
}

static int control_workload_groups(pact_workload_t *wl, int request)
{
    int result = 0;
    for (size_t i = 0; i < wl->nr_threads; i++) {
        if (control_pmu_group(wl->threads[i].leader.fd, request) < 0) {
            result = -1;
        }
    }
    return result;
}

static void fail_pmu_control(pact_context_t *ctx)
{
    ctx->sampling_failed = true;
    ctx->running = false;
}

void start_pmu_perf_events(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    if (ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_RESET) < 0 ||
        ioctl_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha, PERF_EVENT_IOC_RESET) < 0 ||
        control_workload_groups(wl, PERF_EVENT_IOC_RESET) < 0 ||
        ioctl_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha, PERF_EVENT_IOC_ENABLE) < 0 ||
        control_workload_groups(wl, PERF_EVENT_IOC_ENABLE) < 0 ||
        ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_ENABLE) < 0) {
        fail_pmu_control(ctx);
        stop_pmu_perf_events(ctx);
    }
}

void stop_pmu_perf_events(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    /* Do not short-circuit: stop every group even if an earlier stop failed. */
    int cha = ioctl_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha, PERF_EVENT_IOC_DISABLE);
    int tasks = control_workload_groups(wl, PERF_EVENT_IOC_DISABLE);
    int core =
        ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_DISABLE);
    if (cha < 0 || tasks < 0 || core < 0) {
        fail_pmu_control(ctx);
    }
}

void read_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha)
{
    for (int i = 0; i < nr_cha; i++) {
        read_pmu_event_group(&cha_pmus[i].group_fast);
        read_pmu_event_group(&cha_pmus[i].group_slow);
        /* scale readings with time_enabled / time_running */
        scale_multiplexed_events(&cha_pmus[i].group_fast);
        scale_multiplexed_events(&cha_pmus[i].group_slow);
    }
}

/* Read a perf event group via leader_fd. Matches the N values returned by
 * the kernel to the caller's expected event IDs and writes them into
 * values[]; unmatched slots are left zeroed. Returns bytes read (>0 on
 * success, 0 on empty, <0 on error). */
static int read_perf_event_group_raw(int leader_fd, const uint64_t *ids, uint64_t *values, int n,
                                     uint64_t *out_time_enabled, uint64_t *out_time_running,
                                     bool dummy_leader)
{
    _Alignas(read_format_t) char buf[4096];
    read_format_t *rf = (read_format_t *)buf;
    int bytes_read = read(leader_fd, buf, sizeof(buf));
    if (bytes_read <= 0) {
        return bytes_read;
    }
    if ((size_t)bytes_read < sizeof(*rf) ||
        rf->nr > (sizeof(buf) - sizeof(*rf)) / sizeof(rf->values[0]) ||
        sizeof(*rf) + rf->nr * sizeof(rf->values[0]) != (size_t)bytes_read ||
        rf->nr != (uint64_t)n + dummy_leader) {
        errno = EIO;
        return -1;
    }
    for (int j = 0; j < n; j++) {
        values[j] = 0;
    }
    unsigned int found = 0;
    for (uint64_t i = 0; i < rf->nr; i++) {
        for (int j = 0; j < n; j++) {
            if (rf->values[i].id == ids[j]) {
                if (found & (1u << j)) {
                    errno = EIO;
                    return -1;
                }
                found |= 1u << j;
                values[j] = rf->values[i].value;
                break;
            }
        }
    }
    if (found != (1u << n) - 1) {
        errno = EIO;
        return -1;
    }
    if (out_time_enabled) {
        *out_time_enabled = rf->time_enabled;
    }
    if (out_time_running) {
        *out_time_running = rf->time_running;
    }
    return bytes_read;
}

/* Read a perf_event_t[N] group (per_cpu_state_t / pact_workload_t shape):
 * gather ids, call the raw helper, scatter values back. */
static int read_perf_event_array(perf_event_t *leader, perf_event_t *events, int n)
{
    if (leader->fd < 0) {
        return -1;
    }
    uint64_t ids[CORE_EVENT_COUNT];
    uint64_t values[CORE_EVENT_COUNT];
    for (int j = 0; j < n; j++) {
        ids[j] = events[j].id;
    }
    int bytes_read = read_perf_event_group_raw(leader->fd, ids, values, n, &leader->time_enabled,
                                               &leader->time_running, true);
    if (bytes_read <= 0) {
        return bytes_read;
    }
    for (int j = 0; j < n; j++) {
        events[j].value = (events[j].fd >= 0) ? values[j] : 0;
    }
    return bytes_read;
}

/* Read only covered TIDs. A zero-running or failed group invalidates the
 * window rather than presenting partial counts as complete measurements. */
static void read_workload_counting_events(pact_workload_t *wl)
{
    wl->stats.llc_misses_fast = 0;
    wl->stats.llc_misses_slow = 0;
    wl->counters_valid = wl->nr_threads > 0;
    for (size_t i = 0; i < wl->nr_threads; i++) {
        thread_perf_t *t = &wl->threads[i];
        int n = read_perf_event_array(&t->leader, t->events, CORE_EVENT_COUNT);
        t->valid = n > 0;
        if (!t->valid) {
            wl->counters_valid = false;
            continue;
        }
        uint64_t enabled = t->leader.time_enabled - t->last_enabled;
        uint64_t running = t->leader.time_running - t->last_running;
        t->last_enabled = t->leader.time_enabled;
        t->last_running = t->leader.time_running;
        if (!running) {
            t->valid = false;
            continue;
        }
        /* Reject multiplexed windows. Extrapolating unequal thread coverage
         * can bias attribution even if each raw count is scaled. */
        if (running != enabled) {
            wl->counters_valid = false;
        }
        wl->stats.time_running += running;
        wl->stats.llc_misses_fast += t->events[CORE_EVENT_LLC_MISS_FAST].value;
        wl->stats.llc_misses_slow += t->events[CORE_EVENT_LLC_MISS_SLOW].value;
    }
}

int setup_dummy_leader_event(perf_event_t *perf_event, pid_t pid, int cpu)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_DUMMY;
    pe.size = sizeof(pe);
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;
    pe.disabled = 1; /* ONLY group leader start disabled*/
    pe.inherit = 0;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    perf_event->fd = perf_event_open(&pe, pid, cpu, -1, 0);
    if (perf_event->fd == -1) {
        log_error("setup_dummy_leader_event", "perf_event_open(pid=%d, cpu=%d) failed: %s", pid,
                  cpu, strerror(errno));
        return -1;
    }

    return 0;
}

/* Setup PEBS for LLC miss sampling, for the target CPUs */
int setup_pebs_event(per_cpu_state_t *cpu_state, pid_t pid, int cpu)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.sample_period = cpu_state->pebs_sampling_period;
    pe.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.exclude_idle = 1;
    pe.mmap = 1;
    pe.precise_ip = 2; /* Request PEBS */
    pe.inherit = 0;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    /* MEM_LOAD_L3_MISS_RETIRED.REMOTE_DRAM — slow tier only */
    pe.config = g_pmu_platform.event_llc_miss_remote;

    cpu_state->fd_pebs = perf_event_open(&pe, pid, cpu, cpu_state->leader.fd, 0);
    if (cpu_state->fd_pebs < 0) {
        if (errno == EACCES) {
            log_error("setup_pebs_sampling",
                      "Permission denied for PEBS - run as root or adjust perf_event_paranoid");
        } else if (errno == ENODEV) {
            log_error("setup_pebs_sampling", "PEBS not supported on this hardware");
        } else {
            log_error("setup_pebs_sampling", "Failed to open PEBS event");
        }
        return -1;
    }
    log_info("setup_pebs_sampling", "CPU [%d] PEBS event fd:%d (REMOTE_DRAM)", cpu,
             cpu_state->fd_pebs);

    /* Map the buffer */
    size_t mmap_size = (1 + PERF_BUFFER_PAGES) * PAGE_SIZE;
    cpu_state->pebs_mmap =
        mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu_state->fd_pebs, 0);
    if (cpu_state->pebs_mmap == MAP_FAILED) {
        log_error("setup_pebs_sampling", "Failed to mmap PEBS buffer");
        safe_close(cpu_state->fd_pebs, "setup_pebs_sampling");
        cpu_state->fd_pebs = -1;
        return -1;
    }

    return 0;
}

int setup_counting_event(perf_event_t *perf_event, pid_t pid, int cpu, perf_event_t *leader,
                         uint64_t config, const char *name)
{
    struct perf_event_attr pe;

    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;
    pe.config = config;                      /* event config */
    pe.sample_type = PERF_SAMPLE_IDENTIFIER; /* key for counting mode */
    pe.sample_period = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.inherit = 0;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    /* CRITICAL: Leader must start disabled, members inherit this */
    if (leader == NULL) {
        pe.disabled = 1; /* This is the leader - start disabled */
    } else {
        pe.disabled = 0; /* Group member - will follow leader's state */
    }

    perf_event->fd = perf_event_open(&pe, pid, cpu, (leader == NULL ? -1 : leader->fd), 0x8);
    if (perf_event->fd < 0) {
        log_error("setup_counting_event", "Failed to open counting event config=0x%llx", config);
        perf_event->fd = -1;
        return -1;
    } else {
        log_info("setup_counting_event", "thread [%d] counting event %s (config=0x%llx) fd:%d", pid,
                 name, config, perf_event->fd);
    }
    if (ioctl(perf_event->fd, PERF_EVENT_IOC_ID, &perf_event->id) < 0) {
        int error = errno;
        close(perf_event->fd);
        perf_event->fd = -1;
        errno = error;
        return -1;
    }

    return 0;
}


/* Read an event group */
int read_pmu_event_group(event_group_t *event_group)
{
    event_group->read_valid = false;
    memset(event_group->values, 0, sizeof(event_group->values));
    if (event_group->fds[0] < 0) {
        event_group->needs_resync = true;
        return -1;
    }
    uint64_t te = 0, tr = 0;
    int bytes_read =
        read_perf_event_group_raw(event_group->fds[0], event_group->ids, event_group->values,
                                  event_group->counters_used, &te, &tr, false);
    if (bytes_read <= 0) {
        memset(event_group->values, 0, sizeof(event_group->values));
        event_group->needs_resync = true;
        return -1;
    }

    bool resync = event_group->needs_resync;
    uint64_t previous_enabled = event_group->time_enabled;
    uint64_t previous_running = event_group->time_running;
    event_group->last_time_enabled = previous_enabled;
    event_group->last_time_running = previous_running;
    event_group->time_enabled = te;
    event_group->time_running = tr;
    event_group->needs_resync = false;
    /* RESET clears counts but not scheduling time. After a failed read, the
     * next successful read only restores a boundary for the following window. */
    if (resync || te < previous_enabled || tr < previous_running ||
        tr - previous_running > te - previous_enabled || tr > te) {
        memset(event_group->values, 0, sizeof(event_group->values));
        return -1;
    }
    event_group->read_valid = true;
    return 0;
}

void scale_multiplexed_events(event_group_t *event_group)
{
    if (!event_group->read_valid) {
        return;
    }
    double scale_factor = 1.0;
    if (event_group->time_running - event_group->last_time_running == 0) {
        return; /* No change in time_running, no scaling needed */
    }
    scale_factor = (double)(event_group->time_enabled - event_group->last_time_enabled) /
                   (double)(event_group->time_running - event_group->last_time_running);
    for (int i = 0; i < event_group->counters_used; i++) {
        event_group->values[i] = (uint64_t)((double)event_group->values[i] * scale_factor);
    }
}

/*
 * Per-tier MLP from CHA TOR counters (Algorithm 1): MLP = ΔT1/ΔT2, where
 * T1 accumulates TOR occupancy and T2 counts cycles with at least one
 * outstanding TOR entry. Occupancy and cycle deltas are summed across all
 * of the workload's CHAs before dividing, so each tier yields one ratio
 * per sampling window. Counters are IOC_RESET at window start, so group
 * values are true window deltas; each group's values were already
 * multiplex-corrected by scale_multiplexed_events(), so summing across
 * groups with different schedule fractions is sound.
 */
static double calculate_tier_mlp(pact_workload_t *wl, int tier)
{
    uint64_t sum_occupancy = 0;
    uint64_t sum_cycles = 0;
    int valid_groups = 0;

    for (int cha = 0; cha < wl->nr_cha; cha++) {
        event_group_t *group =
            (tier == 0) ? &wl->cha_pmus[cha].group_fast : &wl->cha_pmus[cha].group_slow;

        /* Reject incomplete coverage or groups scheduled under 1 ms; their
         * scaled-up readings are noise. time_running is cumulative since
         * open (IOC_RESET clears only counter values), so the window's
         * share is the delta against the previous read. */
        if (!group->read_valid || group->time_running - group->last_time_running < 1000000) {
            return -1.0;
        }
        valid_groups++;
        sum_occupancy += group->values[CHA_TOR_OCCUPANCY];
        sum_cycles += group->values[CHA_TOR_CYCLES];
    }

    if (valid_groups == 0) {
        return -1.0; /* no valid measurement this window (all multiplexed out) */
    }
    if (sum_occupancy == 0 || sum_cycles == 0) {
        return g_pmu_platform.mlp_min; /* measured, but no traffic to this tier */
    }

    double mlp = (double)sum_occupancy / (double)sum_cycles;
    if (mlp < g_pmu_platform.mlp_min) {
        mlp = g_pmu_platform.mlp_min;
    }
    if (mlp > g_pmu_platform.mlp_max) {
        mlp = g_pmu_platform.mlp_max;
    }
    return mlp;
}

/* Attribute samples only when both tiers have fresh, complete CHA coverage. */
static void calculate_workload_mlp(pact_workload_t *wl)
{
    double fast = calculate_tier_mlp(wl, 0);
    double slow = calculate_tier_mlp(wl, 1);

    if (fast < 0.0 || slow < 0.0) {
        wl->counters_valid = false;
        return;
    }
    wl->workload_mlp_fast = fast;
    wl->workload_mlp_slow = slow;
}

/* Sum task-local counts only from covered groups; MLP uses CHA TOR ratios. */
void read_pmu_counting_events(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;

    read_workload_counting_events(wl);
    read_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha);
    calculate_workload_mlp(wl);
    log_debug("read_pmu_counting_events", "Workload (PID %d) MLP: fast=%.2f, slow=%.2f",
              wl->target_pid, wl->workload_mlp_fast, wl->workload_mlp_slow);
}

static void close_thread_events(thread_perf_t *t)
{
    if (t->leader.fd >= 0) {
        close(t->leader.fd);
    }
    for (int j = 0; j < CORE_EVENT_COUNT; j++) {
        if (t->events[j].fd >= 0) {
            close(t->events[j].fd);
        }
    }
    t->leader.fd = -1;
}

void cleanup_workload_counting_events(pact_workload_t *wl)
{
    for (size_t i = 0; i < wl->nr_threads; i++) {
        close_thread_events(&wl->threads[i]);
    }
    free(wl->threads);
    wl->threads = NULL;
    wl->nr_threads = 0;
}

static int compare_thread_id(const void *a, const void *b)
{
    pid_t x = ((const thread_perf_t *)a)->tid, y = ((const thread_perf_t *)b)->tid;
    return (x > y) - (x < y);
}

bool workload_covers_tid(const pact_workload_t *wl, pid_t tid)
{
    if (!wl->nr_threads) {
        return false;
    }
    thread_perf_t key = {.tid = tid};
    const thread_perf_t *t =
        bsearch(&key, wl->threads, wl->nr_threads, sizeof(*wl->threads), compare_thread_id);
    return t && t->valid;
}

static int thread_start_time(pid_t pid, pid_t tid, uint64_t *value)
{
    char path[96], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    bool ok = fgets(line, sizeof(line), fp) != NULL;
    fclose(fp);
    char *end = ok ? strrchr(line, ')') : NULL;
    if (!end) {
        errno = EIO;
        return -1;
    }
    char *save, *token = strtok_r(end + 1, " ", &save);
    if (token && (token[0] == 'Z' || token[0] == 'X')) {
        errno = ESRCH;
        return -1;
    }
    for (int field = 3; token && field < 22; field++) {
        token = strtok_r(NULL, " ", &save);
    }
    if (!token) {
        errno = EIO;
        return -1;
    }
    *value = strtoull(token, &end, 10);
    if (*end) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int setup_workload_counting_events(pact_workload_t *wl)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", wl->target_pid);
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    thread_perf_t *next = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *de;
    int result = 0;
    while ((de = readdir(dir))) {
        char *end;
        long id = strtol(de->d_name, &end, 10);
        if (*end || id <= 0 || id > INT_MAX) {
            continue;
        }
        thread_perf_t t = {.tid = (pid_t)id, .valid = true};
        init_perf_event(&t.leader);
        for (int j = 0; j < CORE_EVENT_COUNT; j++) {
            init_perf_event(&t.events[j]);
        }
        if (thread_start_time(wl->target_pid, id, &t.start_time) < 0) {
            if (errno == ESRCH || errno == ENOENT) {
                continue;
            }
            result = -1;
            break;
        }
        if (wl->cpu_mask) {
            cpu_set_t affinity;
            if (sched_getaffinity(id, sizeof(affinity), &affinity) < 0) {
                if (errno == ESRCH) {
                    continue;
                }
                result = -1;
                break;
            }
            bool covered = true;
            for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
                if (CPU_ISSET(cpu, &affinity) && (cpu >= 64 || !(wl->cpu_mask & (1ULL << cpu)))) {
                    covered = false;
                }
            }
            if (!covered) {
                log_error("setup_workload_counting_events", "TID %ld left the sampled CPU affinity",
                          id);
                errno = EINVAL;
                result = -1;
                break;
            }
        }
        thread_perf_t *old_entry = wl->nr_threads ? bsearch(&t, wl->threads, wl->nr_threads,
                                                            sizeof(*wl->threads), compare_thread_id)
                                                  : NULL;
        size_t old = old_entry && old_entry->start_time == t.start_time
                         ? (size_t)(old_entry - wl->threads)
                         : wl->nr_threads;
        if (old < wl->nr_threads) {
            t = wl->threads[old];
        } else {
            if (setup_dummy_leader_event(&t.leader, id, -1) < 0) {
                if (errno == ESRCH || errno == ENOENT) {
                    continue;
                }
                result = -1;
                break;
            }
            bool failed = false;
            for (int j = 0; j < CORE_EVENT_COUNT; j++) {
                if (setup_counting_event(&t.events[j], id, -1, &t.leader,
                                         core_event_configs[j].config,
                                         core_event_configs[j].name) < 0) {
                    failed = true;
                    break;
                }
            }
            if (failed) {
                int error = errno;
                close_thread_events(&t);
                if (error == ESRCH || error == ENOENT) {
                    continue;
                }
                result = -1;
                break;
            }
        }
        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 16;
            thread_perf_t *grown = realloc(next, new_capacity * sizeof(*next));
            if (!grown) {
                if (old == wl->nr_threads) {
                    close_thread_events(&t);
                }
                result = -1;
                break;
            }
            next = grown;
            capacity = new_capacity;
        }
        next[count++] = t;
        if (old < wl->nr_threads) {
            wl->threads[old].leader.fd = -1;
            for (int j = 0; j < CORE_EVENT_COUNT; j++) {
                wl->threads[old].events[j].fd = -1;
            }
        }
    }
    closedir(dir);
    cleanup_workload_counting_events(wl);
    wl->threads = next;
    wl->nr_threads = count;
    if (count) {
        qsort(next, count, sizeof(*next), compare_thread_id);
    }
    return count ? result : -1;
}
