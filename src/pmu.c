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
event_config_t cpu_event_configs[CPU_EVENT_COUNT];

int validate_hardware_access(void)
{
    /* Initialize NUMA */
    if (numa_available() < 0) {
        log_warning("validate_hardware_access", "NUMA not available");
        return 0; /* Fatal: the caller stops the runtime */
    }

    /* Check if we have the necessary permissions for hardware counters */
    if (geteuid() != 0) {
        log_warning("validate_hardware_access",
                    "Running without root privileges: the runtime needs root");
        return 0; /* Fatal: the caller stops the runtime */
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
            cha_pmus[i].group.fds[j] = -1;
            cha_pmus[i].group.ids[j] = -1;
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
        exit(EXIT_FAILURE); /* a /sys read failure stops the runtime */
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
            log_warning("detect_cha_pmus", "CHA ID %d is out of bounds (0-%d), stopping", cha_id,
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

/* Find the smallest pmu_type: used as the origin for CHA-offset → core
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

        /* The group read matches each value by this id. */
        if (ioctl(fd, PERF_EVENT_IOC_ID, &event_group->ids[i]) < 0) {
            log_warning("setup_tor_events", "Cannot read the id of the %s event (tier %d): %s",
                        event_names[i], tier, strerror(errno));
            close(fd);
            return -1;
        }
        event_group->fds[i] = fd;
        event_group->counters_used++;

        if (i == CHA_TOR_OCCUPANCY) {
            leader_fd = fd;
        }
    }

    return 0;
}

/* Close the open counter fds of one event group and mark them -1. */
static void close_event_group_fds(event_group_t *g)
{
    for (int j = 0; j < g->counters_used; j++) {
        if (g->fds[j] != -1) {
            close(g->fds[j]);
            g->fds[j] = -1;
        }
    }
}

/* Open the slow-tier TOR event group for one CHA. With tid_filter the
 * group counts only requests of the CHA's paired core (TID filter on);
 * without it the group counts requests of every core. On failure the
 * partially opened fds are closed and fds[0] is -1; the caller decides
 * whether the run can go on without this CHA. Returns the number of events
 * opened (0 on failure). */
static int setup_one_cha_tor_groups(cha_pmu_info_t *cha, bool tid_filter)
{
    int filter_core = tid_filter ? cha->core_id : -1;
    if (setup_tor_events(&cha->group, cha->pmu_type, PMU_TIER_SLOW, filter_core) == 0) {
        return cha->group.counters_used;
    }
    log_warning("setup_uncore_events", "Failed to set up the TOR events of CHA %d", cha->cha_id);
    close_event_group_fds(&cha->group);
    return 0;
}

int setup_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int *nr_cha, bool tid_filter)
{
    int total_events = 0;
    int active_cha_count = 0;

    for (int i = 0; i < *nr_cha; i++) {
        log_info("setup_uncore_events",
                 "Setting up CHA %d (%s, PMU type %d, core %d, TID filter %s)", cha_pmus[i].cha_id,
                 cha_pmus[i].device_name, cha_pmus[i].pmu_type, cha_pmus[i].core_id,
                 tid_filter ? "on" : "off");
        int n = setup_one_cha_tor_groups(&cha_pmus[i], tid_filter);
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
        if (control_pmu_group(cha_pmus[i].group.fds[0], request) < 0) {
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

/* Apply one group request to the LLC-miss counting groups: per task of
 * every workload (PACT) or per workload CPU (Tierce). Keep going after a
 * failure so DISABLE performs best-effort cleanup. */
static int control_counting_groups(pact_context_t *ctx, int request)
{
    int result = 0;
    if (ctx->mode == PACT_MODE_TIERCE) {
        for (int c = 0; c < ctx->nr_all_cpus; c++) {
            int fd = ctx->cpu_states[c].count_leader.fd;
            if (fd >= 0 && control_pmu_group(fd, request) < 0) {
                result = -1;
            }
        }
        return result;
    }
    for (int w = 0; w < ctx->nr_workloads; w++) {
        if (control_workload_groups(&ctx->workloads[w], request) < 0) {
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

/* Tierce mode stops only the CHA groups for a window's work. The counting
 * groups run free, and the PEBS groups are disabled at the boundary (to
 * flush their buffers) and enabled again at once, so neither is blind
 * while the sampler works. */
void start_pmu_perf_events(pact_context_t *ctx)
{
    bool tierce = ctx->mode == PACT_MODE_TIERCE;
    if ((!tierce &&
         ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_RESET) < 0) ||
        ioctl_pmu_cha_perf_events(ctx->cha_pmus, ctx->nr_cha, PERF_EVENT_IOC_RESET) < 0 ||
        (!tierce && control_counting_groups(ctx, PERF_EVENT_IOC_RESET) < 0) ||
        ioctl_pmu_cha_perf_events(ctx->cha_pmus, ctx->nr_cha, PERF_EVENT_IOC_ENABLE) < 0 ||
        control_counting_groups(ctx, PERF_EVENT_IOC_ENABLE) < 0 ||
        ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_ENABLE) < 0) {
        fail_pmu_control(ctx);
        stop_pmu_perf_events(ctx);
    }
}

void stop_pmu_perf_events(pact_context_t *ctx)
{
    /* Do not short-circuit: stop every group even if an earlier stop failed. */
    bool tierce = ctx->mode == PACT_MODE_TIERCE && !ctx->sampling_failed;
    int cha = ioctl_pmu_cha_perf_events(ctx->cha_pmus, ctx->nr_cha, PERF_EVENT_IOC_DISABLE);
    int tasks = tierce ? 0 : control_counting_groups(ctx, PERF_EVENT_IOC_DISABLE);
    int core =
        ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_DISABLE);
    if (tierce && core == 0) {
        /* The disable flushed the PEBS buffers; sample on. */
        core = ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_ENABLE);
    }
    if (cha < 0 || tasks < 0 || core < 0) {
        fail_pmu_control(ctx);
    }
}

void read_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha)
{
    for (int i = 0; i < nr_cha; i++) {
        read_pmu_event_group(&cha_pmus[i].group);
        /* scale readings with time_enabled / time_running */
        scale_multiplexed_events(&cha_pmus[i].group);
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
    if (leader->fd < 0 || n > MAX_GROUP_EVENTS) {
        return -1;
    }
    uint64_t ids[MAX_GROUP_EVENTS] = {0};
    uint64_t values[MAX_GROUP_EVENTS];
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

/* Scale a window count by enabled/running time. A group that the kernel
 * multiplexed counted only part of the window; the window is still scored. */
uint64_t scale_window_count(uint64_t value, uint64_t enabled, uint64_t running)
{
    if (running == 0 || running >= enabled) {
        return value;
    }
    double scaled = (double)value * ((double)enabled / (double)running);
    return scaled < 18446744073709551615.0 ? (uint64_t)scaled : UINT64_MAX;
}

/* Read only covered TIDs. A failed, malformed or zero-running group removes
 * its thread from this window: the thread leaves the LLC-miss sum, and the
 * sample reader drops its samples (workload_covers_tid). The rest of the
 * window is scored. */
static void read_workload_counting_events(pact_workload_t *wl)
{
    wl->stats.llc_misses_slow = 0;
    for (size_t i = 0; i < wl->nr_threads; i++) {
        thread_perf_t *t = &wl->threads[i];
        int n = read_perf_event_array(&t->leader, t->events, CORE_EVENT_COUNT);
        t->valid = n > 0;
        if (!t->valid) {
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
        wl->stats.time_running += running;
        wl->stats.llc_misses_slow +=
            scale_window_count(t->events[CORE_EVENT_LLC_MISS_SLOW].value, enabled, running);
    }
}

/* CAT feed: add one FG CPU's window counts (unscaled) and clocks to the
 * controller's running sums. */
static void feed_cat_counts(cat_feed_t *feed, const per_cpu_state_t *cs, uint64_t enabled,
                            uint64_t running)
{
    for (int ev = 0; ev < CPU_EVENT_COUNT; ev++) {
        int slot = cs->count_slot[ev];
        if (slot >= 0) {
            feed->count[ev] += cs->count_events[slot].value;
        }
    }
    feed->time_enabled += enabled;
    feed->time_running += running;
}

/* Tierce: read every workload CPU's counting group. A failed, malformed or
 * zero-running group removes the CPU from this window: it leaves its
 * workload's LLC-miss sum, and the sample reader drops its samples
 * (count_valid). A multiplexed group is scaled by enabled/running time.
 * The CAT foreground's CPUs also feed the LLC way controller, which sums
 * its own window counts. The groups run free; a window's count is the
 * difference to the previous read. */
static void read_cpu_counting_groups(pact_context_t *ctx)
{
    for (int w = 0; w < ctx->nr_workloads; w++) {
        ctx->workloads[w].stats.llc_misses_slow = 0;
    }
    cat_feed_t *feed = &ctx->cat.feed;
    for (int c = 0; c < ctx->nr_all_cpus; c++) {
        per_cpu_state_t *cs = &ctx->cpu_states[c];
        bool fg = feed->active && cs->owner == CAT_FG_WORKLOAD;
        cs->count_valid = false;
        cs->llc_misses = 0;
        if (cs->owner < 0 || cs->count_leader.fd < 0) {
            continue;
        }
        if (read_perf_event_array(&cs->count_leader, cs->count_events, cs->nr_count_events) <= 0) {
            feed->read_failed |= fg;
            continue;
        }
        for (int j = 0; j < cs->nr_count_events; j++) {
            uint64_t raw = cs->count_events[j].value;
            cs->count_events[j].value =
                raw >= cs->count_last_value[j] ? raw - cs->count_last_value[j] : 0;
            cs->count_last_value[j] = raw;
        }
        uint64_t enabled = cs->count_leader.time_enabled - cs->count_last_enabled;
        uint64_t running = cs->count_leader.time_running - cs->count_last_running;
        cs->count_last_enabled = cs->count_leader.time_enabled;
        cs->count_last_running = cs->count_leader.time_running;
        if (fg) {
            feed_cat_counts(feed, cs, enabled, running);
        }
        if (!running) {
            continue;
        }
        cs->count_valid = true;
        cs->llc_misses =
            scale_window_count(cs->count_events[CPU_EVENT_LLC_MISS_SLOW].value, enabled, running);
        pact_stats_t *stats = &ctx->workloads[cs->owner].stats;
        stats->time_running += running;
        stats->llc_misses_slow += cs->llc_misses;
    }
}

void calculate_workload_mlp(pact_context_t *ctx)
{
    double sum[MAX_WORKLOADS] = {0};
    int cores[MAX_WORKLOADS] = {0};
    for (int c = 0; c < ctx->nr_all_cpus; c++) {
        per_cpu_state_t *cs = &ctx->cpu_states[c];
        if (cs->owner < 0) {
            continue;
        }
        if (cs->cha >= 0) {
            const event_group_t *group = &ctx->cha_pmus[cs->cha].group;
            if (cha_group_fresh(group)) {
                cs->mlp = mlp_from_counts(group->values[CHA_TOR_OCCUPANCY],
                                          group->values[CHA_TOR_CYCLES]);
            }
        }
        if (cs->mlp <= 0.0) {
            cs->mlp = g_pmu_platform.mlp_min; /* no measurement yet */
        }
        sum[cs->owner] += cs->mlp;
        cores[cs->owner]++;
    }
    for (int w = 0; w < ctx->nr_workloads; w++) {
        ctx->workloads[w].mlp = cores[w] > 0 ? sum[w] / cores[w] : g_pmu_platform.mlp_min;
    }
}

/* Open one event of a CPU's counting group in the next slot. */
static int open_cpu_event(per_cpu_state_t *cs, cpu_event_index_t ev)
{
    int slot = cs->nr_count_events;
    if (setup_counting_event(&cs->count_events[slot], -1, cs->cpu_id, &cs->count_leader,
                             cpu_event_configs[ev].config, cpu_event_configs[ev].name) < 0) {
        return -1;
    }
    cs->count_slot[ev] = slot;
    cs->nr_count_events++;
    return 0;
}

int setup_cpu_counting_group(per_cpu_state_t *cs, uint32_t extra_events)
{
    for (int ev = 0; ev < CPU_EVENT_COUNT; ev++) {
        cs->count_slot[ev] = -1;
    }
    cs->nr_count_events = 0;
    if (setup_dummy_leader_event(&cs->count_leader, -1, cs->cpu_id) < 0) {
        return -1;
    }
    if (open_cpu_event(cs, CPU_EVENT_LLC_MISS_SLOW) < 0) {
        close_cpu_counting_group(cs);
        return -1;
    }
    for (int ev = CPU_EVENT_LLC_MISS_SLOW + 1; ev < CPU_EVENT_COUNT; ev++) {
        if ((extra_events & (1u << ev)) && open_cpu_event(cs, (cpu_event_index_t)ev) < 0) {
            close_cpu_extra_events(cs);
            return 1;
        }
    }
    return 0;
}

void close_cpu_extra_events(per_cpu_state_t *cs)
{
    for (int slot = 1; slot < cs->nr_count_events; slot++) {
        if (cs->count_events[slot].fd >= 0) {
            close(cs->count_events[slot].fd);
            cs->count_events[slot].fd = -1;
        }
    }
    for (int ev = CPU_EVENT_LLC_MISS_SLOW + 1; ev < CPU_EVENT_COUNT; ev++) {
        cs->count_slot[ev] = -1;
    }
    if (cs->nr_count_events > 1) {
        cs->nr_count_events = 1;
    }
}

void close_cpu_counting_group(per_cpu_state_t *cs)
{
    for (int slot = 0; slot < cs->nr_count_events; slot++) {
        if (cs->count_events[slot].fd >= 0) {
            close(cs->count_events[slot].fd);
            cs->count_events[slot].fd = -1;
        }
    }
    for (int ev = 0; ev < CPU_EVENT_COUNT; ev++) {
        cs->count_slot[ev] = -1;
    }
    cs->nr_count_events = 0;
    if (cs->count_leader.fd >= 0) {
        close(cs->count_leader.fd);
        cs->count_leader.fd = -1;
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

int setup_mem_loads_aux_leader(perf_event_t *perf_event, int cpu)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.config = MEM_LOADS_AUX_EVENT;
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    perf_event->fd = perf_event_open(&pe, -1, cpu, -1, 0);
    if (perf_event->fd == -1) {
        log_error("setup_mem_loads_aux_leader", "perf_event_open(cpu=%d) failed: %s", cpu,
                  strerror(errno));
        return -1;
    }
    return 0;
}

/* Setup PEBS for LLC miss sampling, for the target CPUs */
int setup_pebs_event(per_cpu_state_t *cpu_state, pid_t pid, int cpu, bool use_ldlat)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.sample_period = cpu_state->pebs_sampling_period;
    /* No PERF_SAMPLE_TIME: the group needs use_clockid (one clock for all
     * members), and with use_clockid the kernel does not allow TIME in large
     * PEBS. Without large PEBS every sample raises one interrupt. The reader
     * does not use the time. */
    pe.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_TID;
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

    if (use_ldlat) {
        /* LDLAT: loads above the latency floor, with latency and data
         * source; the reader keeps remote-DRAM samples. The struct weight
         * has the instruction latency in var2_w on Sapphire Rapids and
         * later. */
        pe.config = LDLAT_LOADS_EVENT;
        pe.config1 = LDLAT_HW_THRESHOLD;
        pe.sample_type |= PERF_SAMPLE_WEIGHT_STRUCT | PERF_SAMPLE_DATA_SRC;
    } else {
        /* MEM_LOAD_L3_MISS_RETIRED.REMOTE_DRAM: slow tier only */
        pe.config = g_pmu_platform.event_llc_miss_remote;
    }

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
    log_info("setup_pebs_sampling", "CPU [%d] PEBS event fd:%d (%s)", cpu, cpu_state->fd_pebs,
             use_ldlat ? "LDLAT" : "REMOTE_DRAM");

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

/* A CHA reading is fresh when the read succeeded, kept its scheduling-time
 * boundary, and the group ran at least 1 ms of this window. time_running is
 * cumulative since open (IOC_RESET clears only counter values), so the
 * window's share is the delta against the previous read. */
bool cha_group_fresh(const event_group_t *group)
{
    return group->read_valid && group->time_running - group->last_time_running >= 1000000;
}

/* MLP of one ratio: zero occupancy or zero cycles is a measured window
 * without slow-tier traffic and gets mlp_min; otherwise clamp the ratio. */
double mlp_from_counts(uint64_t occupancy, uint64_t cycles)
{
    if (occupancy == 0 || cycles == 0) {
        return g_pmu_platform.mlp_min;
    }
    double mlp = (double)occupancy / (double)cycles;
    if (mlp < g_pmu_platform.mlp_min) {
        mlp = g_pmu_platform.mlp_min;
    }
    if (mlp > g_pmu_platform.mlp_max) {
        mlp = g_pmu_platform.mlp_max;
    }
    return mlp;
}

/*
 * Slow-tier MLP from CHA TOR counters (Algorithm 1): MLP = ΔT1/ΔT2, where
 * T1 accumulates TOR occupancy and T2 counts cycles with at least one
 * outstanding TOR entry. Occupancy and cycle deltas are summed across the
 * fresh CHAs before dividing, so the tier yields one ratio per sampling
 * window, with no smoothing. Counters are IOC_RESET at window start, so
 * group values are true window deltas; each group's values were already
 * multiplex-corrected by scale_multiplexed_events(), so summing across
 * groups with different schedule fractions is sound. A stale, failed or
 * short reading leaves the ratio; when no CHA is fresh the previous value
 * stays. The window is scored either way.
 */
static void calculate_global_mlp(pact_context_t *ctx)
{
    uint64_t sum_occupancy = 0;
    uint64_t sum_cycles = 0;
    int fresh_groups = 0;

    for (int cha = 0; cha < ctx->nr_cha; cha++) {
        event_group_t *group = &ctx->cha_pmus[cha].group;
        if (!cha_group_fresh(group)) {
            continue;
        }
        fresh_groups++;
        sum_occupancy += group->values[CHA_TOR_OCCUPANCY];
        sum_cycles += group->values[CHA_TOR_CYCLES];
    }
    if (fresh_groups > 0) {
        ctx->global_mlp = mlp_from_counts(sum_occupancy, sum_cycles);
    }
}

/* PACT: sum task-local counts only from covered groups; the global MLP
 * uses the CHA TOR ratio of sums. Tierce: sum per-CPU counts of valid CPUs;
 * each workload's MLP is the mean of its cores' MLP. */
void read_pmu_counting_events(pact_context_t *ctx)
{
    if (ctx->mode == PACT_MODE_TIERCE) {
        read_cpu_counting_groups(ctx);
        read_pmu_cha_perf_events(ctx->cha_pmus, ctx->nr_cha);
        calculate_workload_mlp(ctx);
        return;
    }
    for (int w = 0; w < ctx->nr_workloads; w++) {
        read_workload_counting_events(&ctx->workloads[w]);
    }
    read_pmu_cha_perf_events(ctx->cha_pmus, ctx->nr_cha);
    calculate_global_mlp(ctx);
    log_debug("read_pmu_counting_events", "Global MLP: slow=%.2f", ctx->global_mlp);
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

/* thread_start_time() result: the thread exited after the task scan. */
#define THREAD_GONE 1

/* Read the start time (stat field 22) of thread `tid`. Returns 0 with
 * *value set, or THREAD_GONE when the thread exited between the task
 * directory scan and this read: the stat file is gone (ENOENT), the open
 * or the read reports ESRCH, or the state is zombie or dead. Returns -1
 * with errno for an error: EIO for a stat line that does not parse, else
 * the errno of the failed open or read. */
static int thread_start_time(pid_t pid, pid_t tid, uint64_t *value)
{
    char path[96], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return (errno == ENOENT || errno == ESRCH) ? THREAD_GONE : -1;
    }
    errno = 0;
    bool ok = fgets(line, sizeof(line), fp) != NULL;
    int read_error = (!ok && ferror(fp)) ? errno : 0;
    fclose(fp);
    if (read_error == ESRCH) {
        return THREAD_GONE;
    }
    if (read_error != 0) {
        errno = read_error;
        return -1;
    }
    char *end = ok ? strrchr(line, ')') : NULL;
    if (!end) {
        errno = EIO;
        return -1;
    }
    char *save, *token = strtok_r(end + 1, " ", &save);
    if (token && (token[0] == 'Z' || token[0] == 'X')) {
        return THREAD_GONE;
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
        return (errno == ENOENT || errno == ESRCH) ? PACT_WORKLOAD_GONE : -1;
    }
    thread_perf_t *next = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *de;
    int result = 0;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            result = errno ? -1 : 0;
            break;
        }
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
        /* A thread that exits after the scan leaves the new list. It leaves
         * the next window's LLC-miss sum and sample count, and its old groups
         * close once with the old list. */
        int started = thread_start_time(wl->target_pid, id, &t.start_time);
        if (started == THREAD_GONE) {
            continue;
        }
        if (started < 0) {
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
    int saved_error = errno;
    closedir(dir);
    cleanup_workload_counting_events(wl);
    wl->threads = next;
    wl->nr_threads = count;
    if (count) {
        qsort(next, count, sizeof(*next), compare_thread_id);
    }
    if (result < 0) {
        errno = saved_error;
        return -1;
    }
    return count ? 0 : PACT_WORKLOAD_GONE;
}
