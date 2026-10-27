/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/*
 * cat.c: bounded LLC way control through Intel CAT (resctrl).
 *
 * See cat.h for an overview. The control law:
 *
 *     W <- W_max; apply CAT: FG <- W ways, BG <- remainder
 *     every I:
 *         s <- signal(FG)
 *         if      s > tau_hi:  W <- W_max
 *         else if s < tau_lo:  W <- max(W - 1, W_min)
 *         apply CAT: FG <- W ways, BG <- remainder
 *
 * Signals (cat_signal_t):
 *   mpki    LLC misses per 1000 instructions over the last interval, from
 *           the FG's per-CPU counting groups (LONGEST_LAT_CACHE.MISS and
 *           INST_RETIRED.ANY). tau 10 / 25, I = 100 ms.
 *   l2lat   cycles per L2-missing demand load (OFFCORE_REQUESTS_OUTSTANDING /
 *           OFFCORE_REQUESTS, both DEMAND_DATA_RD).
 *   probe   no thresholds: every probe_s seconds withdraw one way and keep
 *           the withdrawal only if the FG's IPC holds within probe_tol;
 *           an IPC collapse restores W_max.
 *   bw      FG memory bandwidth from resctrl MBM (no PMU counters): a
 *           streaming phase (bw above the gate) releases to W_min, a
 *           latency-bound phase (below) protects at W_max.
 *   static  hold W_max (fixed split when W_min == W_max).
 *
 * Nothing here opens a perf event: the counting events are part of the
 * per-CPU groups the runtime already maintains (cat_signal_events() picks
 * them at start), and the counter reader adds the FG's window counts to the
 * feed.
 */

#include <cpuid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cat.h"
#include "error.h"
#include "pact.h"

#define RESCTRL_ROOT "/sys/fs/resctrl"
#define RESCTRL_INFO_L3 RESCTRL_ROOT "/info/L3"
#define RESCTRL_INFO_L3_MON RESCTRL_ROOT "/info/L3_MON"
#define CAT_FG_NAME "tierce_fg"
#define CAT_BG_NAME "tierce_bg"
/* Instruction rate below which the FG carries no demand information. */
#define CAT_IDLE_INSTR_PER_S 1e6
#define CAT_STARTUP_GRACE_S 60.0 /* the dead-feed count waits this long for the FG */
#define CAT_DEAD_FEED_S 10.0     /* no instruction delta this long = dead group */
#define CAT_PROBE_WIN 8          /* probe: intervals per IPC window */
#define CAT_BW_HYST 0.15         /* bw: band as a fraction of the warmup span */
#define CAT_MUX_MIN_RATIO 0.99   /* running/enabled below this = multiplexed */

static const char *const cat_signal_names[CAT_SIGNAL_COUNT] = {
    [CAT_SIGNAL_STATIC] = "static", [CAT_SIGNAL_MPKI] = "mpki", [CAT_SIGNAL_L2LAT] = "l2lat",
    [CAT_SIGNAL_PROBE] = "probe",   [CAT_SIGNAL_BW] = "bw",
};

void cat_default_config(cat_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->signal = CAT_SIGNAL_MPKI;
    cfg->wmin = -1;
    cfg->wmax = -1;
    cfg->interval_ms = 100;
    cfg->tau_lo = -1.0;
    cfg->tau_hi = -1.0;
    cfg->probe_tol = 0.03;
    cfg->probe_s = 2.0;
    cfg->cooldown_s = 3.0;
    cfg->bw_thresh_gbps = 0.0;
    cfg->bw_band_gbps = -1.0;
    cfg->bw_warmup_s = 25.0;
}

void cat_resolve_taus(cat_config_t *cfg)
{
    bool l2lat = cfg->signal == CAT_SIGNAL_L2LAT;
    if (cfg->tau_lo < 0) {
        cfg->tau_lo = l2lat ? CAT_TAU_LO_L2LAT : CAT_TAU_LO_MPKI;
    }
    if (cfg->tau_hi < 0) {
        cfg->tau_hi = l2lat ? CAT_TAU_HI_L2LAT : CAT_TAU_HI_MPKI;
    }
}

int cat_signal_parse(const char *name, cat_signal_t *out)
{
    for (int i = 0; i < CAT_SIGNAL_COUNT; i++) {
        if (strcmp(name, cat_signal_names[i]) == 0) {
            *out = (cat_signal_t)i;
            return 0;
        }
    }
    return -1;
}

const char *cat_signal_name(cat_signal_t sig)
{
    if ((int)sig < 0 || (int)sig >= CAT_SIGNAL_COUNT) {
        return "?";
    }
    return cat_signal_names[sig];
}

int cat_gp_counters(void)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0xA, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
    return (int)((eax >> 8) & 0xff);
}

int cat_signal_events(cat_signal_t sig, bool with_trace, int gp_counters, uint32_t *events)
{
    uint32_t mask = 0;
    switch (sig) {
    case CAT_SIGNAL_MPKI:
        mask = (1u << CPU_EVENT_INSTRUCTIONS) | (1u << CPU_EVENT_LLC_MISS_ALL);
        break;
    case CAT_SIGNAL_L2LAT:
        /* A group that needs every general-purpose counter would be
         * time-shared with the PEBS event, which scales the LLC-miss
         * counts of every window: l2lat needs one counter more. */
        if (gp_counters <= CAT_L2LAT_GP_COUNTERS) {
            log_error("cat_signal_events",
                      "l2lat needs more than %d general-purpose counters per CPU (the PEBS "
                      "event, the slow-tier LLC-miss event and the two OFFCORE events); this "
                      "CPU reports %d (CPUID 0xA). Use --cat-signal mpki or probe here.",
                      CAT_L2LAT_GP_COUNTERS, gp_counters);
            return -1;
        }
        mask = (1u << CPU_EVENT_INSTRUCTIONS) | (1u << CPU_EVENT_OFFCORE_DD_OUT) |
               (1u << CPU_EVENT_OFFCORE_DD);
        break;
    case CAT_SIGNAL_PROBE:
        mask = (1u << CPU_EVENT_INSTRUCTIONS) | (1u << CPU_EVENT_CYCLES);
        break;
    default:
        /* static and bw need no counter; the trace's fg_instr column needs
         * the instructions event. */
        if (with_trace) {
            mask = 1u << CPU_EVENT_INSTRUCTIONS;
        }
        break;
    }
    *events = mask;
    return 0;
}

static void cat_trace_row(cat_state_t *cat, double sig, const char *action);
static bool cat_set_ways(cat_state_t *cat, int w, const char *why);
static void cat_park(cat_state_t *cat, const char *why);

/* ---- small filesystem helpers -------------------------------------- */

/* Write "<val>\n" to path in one write() call (resctrl rejects a schemata
 * write without the newline). Returns 0 on success, -1 else. */
static int cat_write_str(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    char wbuf[640];
    int wlen = snprintf(wbuf, sizeof(wbuf), "%s\n", val);
    if (wlen < 0 || (size_t)wlen >= sizeof(wbuf)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    ssize_t n = write(fd, wbuf, (size_t)wlen);
    int saved = errno;
    close(fd);
    if (n < 0 || n != wlen) {
        errno = saved;
        return -1;
    }
    return 0;
}

/* Read up to buflen-1 bytes from path into buf (NUL-terminated). */
static int cat_read_str(const char *path, char *buf, size_t buflen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, buflen - 1);
    int saved = errno;
    close(fd);
    if (n < 0) {
        errno = saved;
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

static int cat_read_hex(const char *path, uint64_t *out)
{
    char buf[128];
    if (cat_read_str(path, buf, sizeof(buf)) != 0) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    uint64_t v = strtoull(buf, &end, 16);
    if (errno != 0 || end == buf) {
        return -1;
    }
    *out = v;
    return 0;
}

static int cat_read_uint(const char *path, unsigned int *out)
{
    char buf[128];
    if (cat_read_str(path, buf, sizeof(buf)) != 0) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 10);
    if (errno != 0 || end == buf) {
        return -1;
    }
    *out = (unsigned int)v;
    return 0;
}

static int cat_read_u64(const char *path, uint64_t *out)
{
    char buf[128];
    if (cat_read_str(path, buf, sizeof(buf)) != 0) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static double cat_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- resctrl setup helpers ----------------------------------------- */

/* Parse the L3 domain ids from the root schemata ("L3:0=7ff;1=7ff").
 * Returns the number of domains (>= 1) or -1. CDP mode is not handled. */
static int cat_parse_domains(cat_state_t *cat)
{
    char buf[1024];
    if (cat_read_str(RESCTRL_ROOT "/schemata", buf, sizeof(buf)) != 0) {
        return -1;
    }
    char *line = buf;
    char *l3 = NULL;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "L3:", 3) == 0) {
            l3 = p + 3;
            break;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!l3) {
        return -1;
    }
    int n = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(l3, ";", &saveptr);
    while (tok && n < CAT_MAX_DOMAINS) {
        char *end = NULL;
        long id = strtol(tok, &end, 10);
        if (end == tok || *end != '=') {
            return -1;
        }
        cat->domain_ids[n++] = (int)id;
        tok = strtok_r(NULL, ";", &saveptr);
    }
    if (n == 0) {
        return -1;
    }
    cat->nr_domains = n;
    return n;
}

/* Build a resctrl cpus_list ("2,3,4,5") of the CPUs of workloads
 * [start_idx, end_idx). Returns the CPU count, or -1 when the list does not
 * fit (a truncated list would leave CPUs in the default CLOS). */
static int cat_build_cpus_list(pact_context_t *ctx, int start_idx, int end_idx, char *out,
                               size_t out_len)
{
    size_t pos = 0;
    int count = 0;
    out[0] = '\0';
    for (int w = start_idx; w < end_idx && w < ctx->nr_workloads; w++) {
        pact_workload_t *wl = &ctx->workloads[w];
        for (int j = 0; j < wl->nr_target_cpus; j++) {
            int wrote = snprintf(out + pos, out_len - pos, "%s%d", (count == 0) ? "" : ",",
                                 wl->target_cpus[j]);
            if (wrote < 0 || (size_t)wrote >= out_len - pos) {
                return -1;
            }
            pos += (size_t)wrote;
            count++;
        }
    }
    return count;
}

/* Build an "L3:<id>=<mask>;..." line for every domain. */
static void cat_build_schemata(const cat_state_t *cat, uint64_t mask, char *line, size_t len)
{
    size_t p = (size_t)snprintf(line, len, "L3:");
    for (int d = 0; d < cat->nr_domains && p < len; d++) {
        p += (size_t)snprintf(line + p, len - p, "%s%d=%llx", (d == 0) ? "" : ";",
                              cat->domain_ids[d], (unsigned long long)mask);
    }
}

static int cat_write_group_schemata(const cat_state_t *cat, const char *grp, uint64_t mask)
{
    char line[512];
    char path[512];
    cat_build_schemata(cat, mask, line, sizeof(line));
    snprintf(path, sizeof(path), "%s/schemata", grp);
    if (cat_write_str(path, line) != 0) {
        log_warning("cat_apply_split", "Failed to write schemata '%s' to %s: %s", line, path,
                    strerror(errno));
        return -1;
    }
    return 0;
}

/* The groups and masks of a w_fg-way split, in write order: the FG gets
 * the top w_fg ways, the BG the low (num_ways - w_fg) ways. Both masks are
 * contiguous and disjoint. Returns the number of groups. */
static int cat_split_masks(const cat_state_t *cat, int w_fg, const char **grps, uint64_t *masks)
{
    int bg_ways = cat->num_ways - w_fg;
    grps[0] = cat->fg_grp;
    masks[0] = (((uint64_t)1 << w_fg) - 1) << bg_ways;
    grps[1] = cat->bg_grp;
    masks[1] = ((uint64_t)1 << bg_ways) - 1;
    return 2;
}

/* Program a w_fg-way split. When a write fails, the code writes the masks
 * of the current split (cat->w_fg) back to the groups written before it. A
 * failed write-back logs a warning. Before the first split (w_fg 0) there
 * is nothing to put back. Returns 0, or -1 when the split did not change. */
static int cat_apply_split(cat_state_t *cat, int w_fg)
{
    int bg_ways = cat->num_ways - w_fg;
    if (w_fg < (int)cat->min_cbm_bits || bg_ways < (int)cat->min_cbm_bits) {
        return -1;
    }
    const char *grps[2];
    uint64_t masks[2];
    int n = cat_split_masks(cat, w_fg, grps, masks);
    for (int i = 0; i < n; i++) {
        if (cat_write_group_schemata(cat, grps[i], masks[i]) == 0) {
            continue;
        }
        if (cat->w_fg > 0) {
            uint64_t old[2];
            cat_split_masks(cat, cat->w_fg, grps, old);
            for (int j = 0; j < i; j++) {
                if (cat_write_group_schemata(cat, grps[j], old[j]) != 0) {
                    log_warning("cat_apply_split",
                                "Could not restore %s: its ways no longer match W_fg=%d", grps[j],
                                cat->w_fg);
                }
            }
        }
        return -1;
    }
    return 0;
}

/* Sum one monitoring counter over every mon_L3_* domain of a group.
 * Returns 0, or -1 when L3_MON data is unavailable. */
static int cat_sum_mon(const char *grp, const char *name, uint64_t *out)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/mon_data", grp);
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    uint64_t total = 0;
    bool seen = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "mon_L3_", 7) != 0) {
            continue;
        }
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s/%s", dir, de->d_name, name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        uint64_t v;
        if (cat_read_u64(path, &v) == 0) {
            total += v;
            seen = true;
        }
    }
    closedir(d);
    if (!seen) {
        return -1;
    }
    *out = total;
    return 0;
}

static double cat_fg_occupancy_mib(cat_state_t *cat)
{
    uint64_t occ;
    if (!cat->has_mon || cat_sum_mon(cat->fg_grp, "llc_occupancy", &occ) != 0) {
        return -1.0;
    }
    return (double)occ / 1048576.0;
}

/* ---- counter feed -------------------------------------------------- */

/* Running sum of one FG event over the windows read so far. Returns false
 * when the event is not open on every FG CPU. */
static bool cat_fg_counter(const cat_state_t *cat, cpu_event_index_t ev, uint64_t *out)
{
    if (!cat->feed.have[ev]) {
        return false;
    }
    *out = cat->feed.count[ev];
    return true;
}

/* Health of the FG counting groups over the last tick. running < enabled
 * means the kernel multiplexes them: the controller parks at W_max while
 * that lasts. A counter that has not moved for
 * 10 s also parks it. Returns false when the tick must be skipped. d_instr
 * is the instruction delta of this tick (0 = none). */
static bool cat_group_health(pact_context_t *ctx, uint64_t d_instr)
{
    cat_state_t *cat = &ctx->cat;
    double ims = cat->cfg.interval_ms > 0 ? (double)cat->cfg.interval_ms : 100.0;
    uint64_t grace_ticks = (uint64_t)(CAT_STARTUP_GRACE_S * 1000.0 / ims);
    int dead_ticks = (int)(CAT_DEAD_FEED_S * 1000.0 / ims);
    if (dead_ticks < 1) {
        dead_ticks = 1;
    }
    uint64_t en = cat->feed.time_enabled;
    uint64_t run = cat->feed.time_running;
    bool mux = false;
    bool measured =
        cat->have_snapshot && en > cat->last_time_enabled && run >= cat->last_time_running;
    if (measured) {
        double ratio =
            (double)(run - cat->last_time_running) / (double)(en - cat->last_time_enabled);
        mux = ratio < CAT_MUX_MIN_RATIO;
        if (mux && !cat->warned_multiplexed) {
            log_error("cat_control_step",
                      "FG counting groups multiplexed (running %.1f%% of enabled over a tick): "
                      "another PMU user shares the counters; controller parked at W_max while "
                      "it lasts",
                      100.0 * ratio);
            cat->warned_multiplexed = true;
        }
    }
    cat->last_time_enabled = en;
    cat->last_time_running = run;
    /* Stay parked until a measured tick shows the groups whole again; a
     * tick without a measurement is no evidence either way. */
    if (mux || (cat->multiplexed && !measured)) {
        if (!cat->multiplexed) {
            cat->multiplexed = true;
            cat_park(cat, "multiplexed");
        }
        return false;
    }
    if (cat->multiplexed) {
        cat->multiplexed = false;
        log_info("cat_control_step",
                 "FG counting groups no longer multiplexed; controller resumed");
    }
    if (d_instr == 0) {
        /* Startup grace: a workload launched through a shell that sleeps or
         * loads first shows no instructions for a while. The dead-feed
         * count starts once the counters have moved, under a fixed cap. */
        if (!cat->seen_instr && cat->ticks < grace_ticks) {
            if ((cat->ticks % 50) == 1) {
                cat_trace_row(cat, -1.0, "startup");
            }
            return false;
        }
        cat->zero_streak++;
        if ((cat->zero_streak % 20) == 0) {
            log_warning("cat_control_step",
                        "no FG instruction delta for %d ticks (enabled=%llu running=%llu)",
                        cat->zero_streak, (unsigned long long)cat->last_time_enabled,
                        (unsigned long long)cat->last_time_running);
        }
        if (cat->zero_streak == dead_ticks && !cat->signal_dead) {
            log_error("cat_control_step",
                      "FG instruction counter has not moved for %.0f s: the counting group is "
                      "unschedulable or dead; controller parked at W_max",
                      CAT_DEAD_FEED_S);
            cat->signal_dead = true;
            cat_park(cat, "counters-unavailable");
        }
        return false;
    }
    cat->zero_streak = 0;
    cat->seen_instr = true;
    if (cat->signal_dead) {
        cat->signal_dead = false;
        log_info("cat_control_step", "FG counters moving again; controller resumed");
    }
    /* Idle gate: below ~1e6 instructions/s the FG carries no demand
     * information; a ratio over a trickle of instructions is noise. */
    double secs = ims / 1000.0;
    if ((double)d_instr / secs < CAT_IDLE_INSTR_PER_S) {
        if (!cat->warned_low_rate && cat->ticks >= 10) {
            log_warning("cat_control_step",
                        "FG instruction rate %.2e/s is below the idle gate (%.0e/s); the "
                        "controller holds while this lasts",
                        (double)d_instr / secs, CAT_IDLE_INSTR_PER_S);
            cat->warned_low_rate = true;
        }
        cat_trace_row(cat, -1.0, "idle");
        return false;
    }
    return true;
}

/* Delta of a running sum since the previous tick. */
static bool cat_delta(uint64_t now, uint64_t *last, bool have_last, uint64_t *delta)
{
    bool ok = have_last && now >= *last;
    if (ok) {
        *delta = now - *last;
    }
    *last = now;
    return ok;
}

/* A tick after a failed FG CPU read: the feed missed that CPU's window, so
 * take the current sums as the new base and judge nothing. */
static void cat_skip_tick(cat_state_t *cat)
{
    const uint64_t *count = cat->feed.count;
    cat->last_instr = count[CPU_EVENT_INSTRUCTIONS];
    cat->last_cycles = count[CPU_EVENT_CYCLES];
    cat->last_llc_all = count[CPU_EVENT_LLC_MISS_ALL];
    cat->last_off_out = count[CPU_EVENT_OFFCORE_DD_OUT];
    cat->last_off_dd = count[CPU_EVENT_OFFCORE_DD];
    cat->last_time_enabled = cat->feed.time_enabled;
    cat->last_time_running = cat->feed.time_running;
    cat->have_snapshot = true;
    cat_trace_row(cat, -1.0, "read-failed");
}

/* ---- trace --------------------------------------------------------- */

/* Memory traffic of one partition (MB/s) since the previous trace row,
 * from the group's MBM byte counter; -1 without L3_MON or before two
 * readings exist. */
static double cat_trace_mbm(cat_state_t *cat, const char *grp, uint64_t *last, double dt)
{
    uint64_t bytes;
    if (!cat->has_mon || grp[0] == '\0' || cat_sum_mon(grp, "mbm_total_bytes", &bytes) != 0) {
        return -1.0;
    }
    double rate = -1.0;
    if (cat->have_trace_mbm && dt > 0.0 && bytes >= *last) {
        rate = (double)(bytes - *last) / dt / 1e6;
    }
    *last = bytes;
    return rate;
}

static void cat_trace_row(cat_state_t *cat, double sig, const char *action)
{
    double now = cat_now();
    double t = now - cat->t0;
    double occ = cat_fg_occupancy_mib(cat);
    if (cat->trace) {
        double dt = cat->have_trace_mbm ? now - cat->trace_last_mbm_t : 0.0;
        double fg_mbps = cat_trace_mbm(cat, cat->fg_grp, &cat->trace_last_mbm_fg, dt);
        double bg_mbps = cat_trace_mbm(cat, cat->bg_grp, &cat->trace_last_mbm_bg, dt);
        cat->trace_last_mbm_t = now;
        cat->have_trace_mbm = true;
        fprintf(cat->trace, "%.1f,%.3f,%d,", t, sig, cat->w_fg);
        if (occ >= 0.0) {
            fprintf(cat->trace, "%.1f,", occ);
        } else {
            fputs("nan,", cat->trace);
        }
        fprintf(cat->trace, "%s,%llu,", action, (unsigned long long)cat->fg_instr_total);
        if (fg_mbps >= 0.0) {
            fprintf(cat->trace, "%.1f,", fg_mbps);
        } else {
            fputs("nan,", cat->trace);
        }
        if (bg_mbps >= 0.0) {
            fprintf(cat->trace, "%.1f\n", bg_mbps);
        } else {
            fputs("nan\n", cat->trace);
        }
        fflush(cat->trace);
    }
    if (strcmp(action, "hold") != 0 || (cat->ticks % 100) == 0) {
        log_info("cat_control_step", "t=%.1f signal=%.3f W_fg=%d occ=%.1fMiB %s", t, sig, cat->w_fg,
                 occ, action);
    }
}

/* Move to a new FG way count; returns true when the hardware changed. */
static bool cat_set_ways(cat_state_t *cat, int w, const char *why)
{
    if (w > cat->w_max) {
        w = cat->w_max;
    }
    if (w < cat->w_min) {
        w = cat->w_min;
    }
    if (w == cat->w_fg) {
        return false;
    }
    if (cat_apply_split(cat, w) != 0) {
        log_warning("cat_control_step", "could not program W_fg=%d (%s); kept %d", w, why,
                    cat->w_fg);
        return false;
    }
    cat->w_fg = w;
    cat->reallocations++;
    return true;
}

/* Park at W_max (multiplexed or dead counters). A probe trial in flight is
 * abandoned: its IPC window mixes the two states. */
static void cat_park(cat_state_t *cat, const char *why)
{
    cat_set_ways(cat, cat->w_max, why);
    if (cat->signal == CAT_SIGNAL_PROBE) {
        cat->probe_state = CAT_PROBE_HOLD;
        cat->have_ref = false;
        cat->ipc_n = 0;
    }
    cat_trace_row(cat, -1.0, why);
}

/* ---- signal evaluation --------------------------------------------- */

const char *cat_hysteresis_law(cat_state_t *cat, double sig)
{
    const char *action = "hold";
    if (sig > cat->tau_hi) {
        if (cat->w_fg < cat->w_max && cat_set_ways(cat, cat->w_max, "attack")) {
            action = "attack";
        }
    } else if (sig < cat->tau_lo && cat->w_fg > cat->w_min) {
        action = cat_set_ways(cat, cat->w_fg - 1, "shed") ? "shed" : "hold-gated";
    }
    return action;
}

static void cat_hysteresis_step(cat_state_t *cat, double sig)
{
    cat_trace_row(cat, sig, cat_hysteresis_law(cat, sig));
}

/* One-time warning when the signal's counters are not open on the FG. */
static void cat_warn_no_signal(cat_state_t *cat, const char *what)
{
    if (!cat->warned_no_signal) {
        log_warning("cat_control_step", "%s counters unavailable for the FG; holding W_fg=%d", what,
                    cat->w_fg);
        cat->warned_no_signal = true;
    }
}

static void cat_step_mpki(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    uint64_t instr, misses, d_instr = 0, d_miss = 0;
    if (!cat_fg_counter(cat, CPU_EVENT_INSTRUCTIONS, &instr) ||
        !cat_fg_counter(cat, CPU_EVENT_LLC_MISS_ALL, &misses)) {
        cat_warn_no_signal(cat, "MPKI");
        return;
    }
    bool ok_i = cat_delta(instr, &cat->last_instr, cat->have_snapshot, &d_instr);
    bool ok_m = cat_delta(misses, &cat->last_llc_all, cat->have_snapshot, &d_miss);
    bool healthy = cat_group_health(ctx, (ok_i && cat->have_snapshot) ? d_instr : 0);
    cat->have_snapshot = true;
    if (!healthy || !ok_i || !ok_m) {
        return;
    }
    cat_hysteresis_step(cat, 1000.0 * (double)d_miss / (double)d_instr);
}

static void cat_step_l2lat(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    uint64_t out, dd, instr, d_out = 0, d_dd = 0, d_instr = 0;
    if (!cat_fg_counter(cat, CPU_EVENT_OFFCORE_DD_OUT, &out) ||
        !cat_fg_counter(cat, CPU_EVENT_OFFCORE_DD, &dd) ||
        !cat_fg_counter(cat, CPU_EVENT_INSTRUCTIONS, &instr)) {
        cat_warn_no_signal(cat, "OFFCORE");
        return;
    }
    bool ok_o = cat_delta(out, &cat->last_off_out, cat->have_snapshot, &d_out);
    bool ok_d = cat_delta(dd, &cat->last_off_dd, cat->have_snapshot, &d_dd);
    bool ok_i = cat_delta(instr, &cat->last_instr, cat->have_snapshot, &d_instr);
    /* the idle gate and the dead-feed rule work on instructions, as in mpki */
    bool healthy = cat_group_health(ctx, (ok_i && cat->have_snapshot) ? d_instr : 0);
    cat->have_snapshot = true;
    if (!healthy || !ok_o || !ok_d || !ok_i || d_dd == 0) {
        return;
    }
    /* Little's law: occupancy-cycles / completions = cycles per L2 miss */
    cat_hysteresis_step(cat, (double)d_out / (double)d_dd);
}

static double cat_median(const double *v, int n)
{
    double s[CAT_PROBE_WIN_MAX];
    for (int i = 0; i < n; i++) {
        double x = v[i];
        int j = i;
        while (j > 0 && s[j - 1] > x) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = x;
    }
    return s[n / 2];
}

static void cat_step_probe(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    uint64_t instr, cyc, d_i = 0, d_c = 0;
    if (!cat_fg_counter(cat, CPU_EVENT_INSTRUCTIONS, &instr) ||
        !cat_fg_counter(cat, CPU_EVENT_CYCLES, &cyc)) {
        cat_warn_no_signal(cat, "IPC");
        return;
    }
    bool ok_i = cat_delta(instr, &cat->last_instr, cat->have_snapshot, &d_i);
    bool ok_c = cat_delta(cyc, &cat->last_cycles, cat->have_snapshot, &d_c);
    bool healthy = cat_group_health(ctx, (ok_i && cat->have_snapshot) ? d_i : 0);
    cat->have_snapshot = true;
    if (!healthy || !ok_i || !ok_c || d_c == 0) {
        return;
    }
    cat->ipc_win[cat->ipc_n++] = (double)d_i / (double)d_c;
    if (cat->ipc_n < cat->probe_win) {
        return;
    }
    double ipc = cat_median(cat->ipc_win, cat->ipc_n);
    cat->ipc_n = 0;
    double now = cat_now();
    const char *action = "hold";

    switch (cat->probe_state) {
    case CAT_PROBE_WARMUP:
        cat->ref_ipc = ipc;
        cat->have_ref = true;
        cat->probe_state = CAT_PROBE_HOLD;
        cat->next_probe_t = now + cat->probe_s;
        action = "warmup";
        break;
    case CAT_PROBE_HOLD:
        if (cat->have_ref && ipc < cat->ref_ipc * (1.0 - 3.0 * cat->probe_tol) &&
            cat->w_fg < cat->w_max) {
            /* a cache-hungry phase started: recover fully */
            cat_set_ways(cat, cat->w_max, "recover");
            cat->have_ref = false;
            action = "recover";
        } else if (!cat->have_ref) {
            cat->ref_ipc = ipc;
            cat->have_ref = true;
        } else if (now >= cat->next_probe_t && cat->w_fg > cat->w_min) {
            cat->ipc_before = ipc;
            if (cat_set_ways(cat, cat->w_fg - 1, "probe-shed")) {
                cat->probe_state = CAT_PROBE_PROBING;
                action = "probe-shed";
            }
        } else if (ipc > cat->ref_ipc) {
            cat->ref_ipc = ipc; /* track the best un-starved IPC */
        }
        break;
    case CAT_PROBE_PROBING:
        if (ipc < cat->ipc_before * (1.0 - cat->probe_tol)) {
            cat_set_ways(cat, cat->w_fg + 1, "restore");
            cat->ref_ipc = cat->ipc_before;
            cat->next_probe_t = now + cat->cooldown_s;
            action = "restore";
        } else {
            cat->ref_ipc = ipc; /* way not needed: the BG keeps it */
            cat->next_probe_t = now + cat->probe_s;
            action = "keep-shed";
        }
        cat->probe_state = CAT_PROBE_HOLD;
        break;
    }
    cat_trace_row(cat, ipc, action);
}

/* bw: calibrate the gate over the warmup (midpoint of the observed range)
 * unless --cat-bw-thresh gave one. Returns the action of the tick. */
static const char *cat_bw_calibrate(pact_context_t *ctx, double gbps, double now)
{
    cat_state_t *cat = &ctx->cat;
    if (cat->bw_thresh_gbps > 0.0) {
        /* Explicit gate: default band of 10% of the gate so a workload at
         * the gate does not flap every tick. */
        cat->bw_calibrated = true;
        if (cat->bw_band_gbps < 0.0) {
            cat->bw_band_gbps = 0.1 * cat->bw_thresh_gbps;
        }
        log_info("cat_control_step", "bw gate explicit: thr=%.2f band=%.2f GB/s",
                 cat->bw_thresh_gbps, cat->bw_band_gbps);
        return NULL;
    }
    if (cat->bw_nsamples < CAT_BW_SAMPLES_MAX) {
        cat->bw_samples[cat->bw_nsamples++] = gbps;
    }
    if (now - cat->t0 < cat->bw_warmup_s || cat->bw_nsamples <= 10) {
        return "warmup";
    }
    double lo = cat->bw_samples[0], hi = cat->bw_samples[0];
    for (int i = 1; i < cat->bw_nsamples; i++) {
        lo = cat->bw_samples[i] < lo ? cat->bw_samples[i] : lo;
        hi = cat->bw_samples[i] > hi ? cat->bw_samples[i] : hi;
    }
    if (hi - lo < 1e-6) {
        log_error("cat_control_step", "FG bandwidth shows no variation over the warmup: this "
                                      "workload has no bandwidth-separable phase; the bw gate "
                                      "stays at W_max");
        cat->bw_thresh_gbps = 1e30;
    } else {
        cat->bw_thresh_gbps = lo + 0.5 * (hi - lo);
        if (cat->bw_band_gbps < 0.0) {
            cat->bw_band_gbps = cat->bw_hyst * (hi - lo);
        }
    }
    cat->bw_calibrated = true;
    log_info("cat_control_step", "bw gate calibrated: lo=%.2f hi=%.2f thr=%.2f band=%.2f GB/s", lo,
             hi, cat->bw_thresh_gbps, cat->bw_band_gbps);
    return "calibrated";
}

static void cat_step_bw(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    uint64_t bytes;
    if (!cat->has_mon || cat_sum_mon(cat->fg_grp, "mbm_total_bytes", &bytes) != 0) {
        if (!cat->warned_no_signal) {
            log_warning("cat_control_step", "MBM unavailable (no L3_MON); holding W_fg=%d",
                        cat->w_fg);
            cat->warned_no_signal = true;
        }
        return;
    }
    double now = cat_now();
    if (!cat->have_mbm) {
        cat->last_mbm_bytes = bytes;
        cat->last_t = now;
        cat->have_mbm = true;
        return;
    }
    double dt = now - cat->last_t;
    if (dt <= 0.0 || bytes < cat->last_mbm_bytes) {
        cat->last_mbm_bytes = bytes;
        cat->last_t = now;
        return;
    }
    double gbps = (double)(bytes - cat->last_mbm_bytes) / dt / 1e9;
    cat->last_mbm_bytes = bytes;
    cat->last_t = now;
    if (!cat->bw_calibrated) {
        const char *action = cat_bw_calibrate(ctx, gbps, now);
        if (action) {
            cat_trace_row(cat, gbps, action);
            return;
        }
    }
    const char *action = "hold";
    if (gbps > cat->bw_thresh_gbps + cat->bw_band_gbps && cat->w_fg != cat->w_min) {
        if (cat_set_ways(cat, cat->w_min, "release")) {
            action = "release"; /* streaming: cache is worthless */
        }
    } else if (gbps < cat->bw_thresh_gbps - cat->bw_band_gbps && cat->w_fg != cat->w_max) {
        if (cat_set_ways(cat, cat->w_max, "protect")) {
            action = "protect"; /* chasing: cache is valuable */
        }
    }
    cat_trace_row(cat, gbps, action);
}

/* ---- teardown at exit() --------------------------------------------- */

/* cat_cleanup() at exit() for a path that skips the normal teardown, so
 * the CLOS groups and a partitioned LLC do not outlive the process. */
static pact_context_t *g_cat_atexit_ctx = NULL;

static void cat_atexit_cleanup(void)
{
    if (g_cat_atexit_ctx) {
        cat_cleanup(g_cat_atexit_ctx);
    }
}

/* ---- public API ---------------------------------------------------- */

/* Read the L3 CAT capabilities. Returns 0, or -1 when CAT cannot run. */
static int cat_read_caps(cat_state_t *cat)
{
    if (access(RESCTRL_INFO_L3, F_OK) != 0) {
        log_warning("cat_init", "No L3 CAT (info/L3 absent); CAT disabled");
        return -1;
    }
    if (cat_read_hex(RESCTRL_INFO_L3 "/cbm_mask", &cat->full_mask) != 0 || cat->full_mask == 0) {
        log_warning("cat_init", "Could not read L3 cbm_mask; CAT disabled");
        return -1;
    }
    if (cat_read_uint(RESCTRL_INFO_L3 "/min_cbm_bits", &cat->min_cbm_bits) != 0) {
        log_warning("cat_init", "min_cbm_bits unreadable; assuming 1");
        cat->min_cbm_bits = 1;
    }
    if (cat->min_cbm_bits < 1) {
        cat->min_cbm_bits = 1;
    }
    if (cat_read_uint(RESCTRL_INFO_L3 "/num_closids", &cat->num_closids) != 0) {
        log_warning("cat_init", "num_closids unreadable; skipping the CLOS count check");
        cat->num_closids = 0;
    }
    cat->num_ways = __builtin_popcountll(cat->full_mask);
    if (cat->num_ways < 2) {
        log_warning("cat_init", "Degenerate L3 (%d ways); CAT disabled", cat->num_ways);
        return -1;
    }
    /* The default CLOS plus the two groups. */
    if (cat->num_closids != 0 && cat->num_closids < 3) {
        log_warning("cat_init", "Too few CLOS ids (%u); CAT disabled", cat->num_closids);
        return -1;
    }
    cat->has_mon = access(RESCTRL_INFO_L3_MON, F_OK) == 0;
    if (cat->cfg.signal == CAT_SIGNAL_BW && !cat->has_mon) {
        log_warning("cat_init", "bw signal needs L3_MON (CMT/MBM); not available on this "
                                "kernel/boot (rdt=cmt,mbmtotal,mbmlocal,l3cat); CAT disabled");
        return -1;
    }
    if (cat_parse_domains(cat) < 0) {
        log_warning("cat_init", "Could not parse L3 domains; CAT disabled");
        return -1;
    }
    return 0;
}

/* Assign the FG's CPUs to tierce_fg and the other workloads' CPUs to
 * tierce_bg. Returns 0, or -1 when CAT cannot run. */
static int cat_assign_cpus(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    char cpus[1024];
    char path[512];
    int fg_cpus =
        cat_build_cpus_list(ctx, CAT_FG_WORKLOAD, CAT_FG_WORKLOAD + 1, cpus, sizeof(cpus));
    if (fg_cpus <= 0) {
        log_warning("cat_init", "FG workload has no pinned CPUs (or too many); CAT disabled");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/cpus_list", cat->fg_grp);
    if (cat_write_str(path, cpus) != 0) {
        log_warning("cat_init", "Failed to assign FG CPUs '%s' (%s); CAT disabled", cpus,
                    strerror(errno));
        return -1;
    }
    log_info("cat_init", "FG (workload %d) CPUs -> %s: %s", CAT_FG_WORKLOAD, CAT_FG_NAME, cpus);

    int bg_cpus =
        cat_build_cpus_list(ctx, CAT_FG_WORKLOAD + 1, ctx->nr_workloads, cpus, sizeof(cpus));
    if (bg_cpus < 0) {
        log_warning("cat_init", "BG CPU list too long; CAT disabled");
        return -1;
    }
    if (bg_cpus > 0) {
        snprintf(path, sizeof(path), "%s/cpus_list", cat->bg_grp);
        if (cat_write_str(path, cpus) != 0) {
            log_warning("cat_init", "Failed to assign BG CPUs '%s' (%s); CAT disabled", cpus,
                        strerror(errno));
            return -1;
        }
        log_info("cat_init", "BG CPUs -> %s: %s", CAT_BG_NAME, cpus);
    } else if (ctx->nr_workloads == 1) {
        log_warning("cat_init", "One workload: nothing to partition against; CAT disabled");
        return -1;
    } else {
        log_warning("cat_init", "BG workloads have no pinned CPUs; CAT disabled");
        return -1;
    }
    return 0;
}

/* Set the way range: explicit bounds must fit min_cbm_bits..ways -
 * min_cbm_bits (returns -2 otherwise); default bounds are (ways + 2) / 4
 * and 3 * ways / 4, clamped into that range. */
static int cat_set_range(cat_state_t *cat)
{
    const cat_config_t *cfg = &cat->cfg;
    int w_lo = (int)cat->min_cbm_bits;
    int w_hi = cat->num_ways - (int)cat->min_cbm_bits;
    if ((cfg->wmin > 0 && (cfg->wmin < w_lo || cfg->wmin > w_hi)) ||
        (cfg->wmax > 0 && (cfg->wmax < w_lo || cfg->wmax > w_hi))) {
        log_error("cat_init",
                  "--cat-wmin/--cat-wmax must be in %d..%d on this cache (%d ways, "
                  "min_cbm_bits=%u)",
                  w_lo, w_hi, cat->num_ways, cat->min_cbm_bits);
        return -2;
    }
    cat->w_max = cfg->wmax > 0 ? cfg->wmax : cat->num_ways * 3 / 4; /* 11 -> 8, 20 -> 15 */
    if (cat->w_max > w_hi) {
        cat->w_max = w_hi;
    }
    cat->w_min = cfg->wmin > 0 ? cfg->wmin : (cat->num_ways + 2) / 4; /* 11 -> 3, 20 -> 5 */
    if (cat->w_min < w_lo) {
        cat->w_min = w_lo;
    }
    if (cat->w_min > cat->w_max) {
        if (cfg->wmin > 0 && cfg->wmax > 0) {
            log_error("cat_init", "--cat-wmin %d exceeds --cat-wmax %d", cfg->wmin, cfg->wmax);
            return -2;
        }
        /* one bound is automatic: it yields to the explicit one */
        if (cfg->wmax > 0) {
            cat->w_min = cat->w_max;
        } else {
            cat->w_max = cat->w_min;
        }
    }
    return 0;
}

/* Create a CLOS group and set *created. A group that exists already is not
 * taken over: another run may use it, and the cleanup would remove it.
 * Returns 0, -1 when mkdir fails, or -2 when the group exists. */
static int cat_make_group(const char *grp, bool *created)
{
    if (mkdir(grp, 0755) == 0) {
        *created = true;
        return 0;
    }
    if (errno == EEXIST) {
        log_error("cat_init",
                  "The resctrl group %s exists already. Another run may use it. If no run "
                  "uses it, remove it with rmdir %s and start again.",
                  grp, grp);
        return -2;
    }
    log_warning("cat_init", "mkdir %s failed (%s). CAT disabled.", grp, strerror(errno));
    return -1;
}

/* Remove the CLOS groups that this run created. Removing a group returns
 * its CPUs and closid to the default group. */
static void cat_remove_groups(cat_state_t *cat)
{
    if (cat->bg_created && rmdir(cat->bg_grp) != 0) {
        log_warning("cat_cleanup", "rmdir %s failed: %s", cat->bg_grp, strerror(errno));
    }
    cat->bg_created = false;
    if (cat->fg_created && rmdir(cat->fg_grp) != 0) {
        log_warning("cat_cleanup", "rmdir %s failed: %s", cat->fg_grp, strerror(errno));
    }
    cat->fg_created = false;
}

/* The feed holds an event only when it is open on every FG CPU. */
static void cat_start_feed(pact_context_t *ctx)
{
    cat_feed_t *feed = &ctx->cat.feed;
    memset(feed, 0, sizeof(*feed));
    for (int ev = 0; ev < CPU_EVENT_COUNT; ev++) {
        bool all = false;
        for (int c = 0; c < ctx->nr_all_cpus; c++) {
            const per_cpu_state_t *cs = &ctx->cpu_states[c];
            if (cs->owner != CAT_FG_WORKLOAD) {
                continue;
            }
            all = cs->count_slot[ev] >= 0;
            if (!all) {
                break;
            }
        }
        feed->have[ev] = all;
    }
    feed->active = true;
}

int cat_init(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    cat_config_t cfg = cat->cfg;
    uint32_t events = cat->events;
    memset(cat, 0, sizeof(*cat));
    cat->cfg = cfg;
    cat->events = events;
    if (!cfg.enabled) {
        return -1;
    }
    if (g_cat_atexit_ctx == NULL) {
        g_cat_atexit_ctx = ctx;
        atexit(cat_atexit_cleanup);
    }

    if (access(RESCTRL_INFO_L3, F_OK) != 0) {
        if (mount("resctrl", RESCTRL_ROOT, "resctrl", 0, NULL) != 0) {
            log_warning("cat_init", "resctrl mount failed (%s); CAT disabled", strerror(errno));
            return -1;
        }
        cat->mounted_by_us = true;
        log_info("cat_init", "Mounted resctrl at " RESCTRL_ROOT);
    }
    int rc = -1;
    if (cat_read_caps(cat) != 0) {
        goto fail_unmount;
    }
    snprintf(cat->fg_grp, sizeof(cat->fg_grp), "%s/%s", RESCTRL_ROOT, CAT_FG_NAME);
    snprintf(cat->bg_grp, sizeof(cat->bg_grp), "%s/%s", RESCTRL_ROOT, CAT_BG_NAME);
    rc = cat_make_group(cat->fg_grp, &cat->fg_created);
    if (rc != 0) {
        goto fail_unmount;
    }
    rc = cat_make_group(cat->bg_grp, &cat->bg_created);
    if (rc != 0) {
        goto fail_rmdir;
    }
    rc = -1;
    if (cat_assign_cpus(ctx) != 0) {
        goto fail_rmdir;
    }
    rc = cat_set_range(cat);
    if (rc != 0) {
        goto fail_rmdir;
    }
    rc = -1;

    cat->signal = cfg.signal;
    cat->tau_lo = cfg.tau_lo;
    cat->tau_hi = cfg.tau_hi;
    cat->probe_tol = cfg.probe_tol;
    cat->probe_win = CAT_PROBE_WIN;
    cat->probe_s = cfg.probe_s;
    cat->cooldown_s = cfg.cooldown_s;
    cat->bw_thresh_gbps = cfg.bw_thresh_gbps;
    cat->bw_band_gbps = cfg.bw_band_gbps;
    cat->bw_hyst = CAT_BW_HYST;
    cat->bw_warmup_s = cfg.bw_warmup_s;
    cat->t0 = cat_now();
    cat->last_t = cat->t0;

    if (cfg.trace_file[0] != '\0') {
        cat->trace = fopen(cfg.trace_file, "w");
        if (!cat->trace) {
            log_warning("cat_init", "cannot open --cat-trace %s: %s", cfg.trace_file,
                        strerror(errno));
        } else {
            fprintf(cat->trace, "t,sig,ways,occ_mib,action,fg_instr,fg_mbps,bg_mbps\n");
            fflush(cat->trace);
        }
    }
    if (cat_apply_split(cat, cat->w_max) != 0) {
        log_warning("cat_init", "Failed to program the initial split; CAT disabled");
        goto fail_rmdir;
    }
    cat->w_fg = cat->w_max; /* start at W_max */

    cat_start_feed(ctx);
    cat->enabled = true;
    printf("CAT: %d LLC ways, FG ways W_min=%d W_max=%d, signal %s, interval %u ms\n",
           cat->num_ways, cat->w_min, cat->w_max, cat_signal_name(cat->signal), cfg.interval_ms);
    log_info("cat_init",
             "CAT enabled: %d ways, %d domains, min_cbm_bits=%u, mon=%s, signal=%s, range "
             "[%d,%d], W_fg=%d, tau=[%.2f,%.2f], interval=%u ms",
             cat->num_ways, cat->nr_domains, cat->min_cbm_bits, cat->has_mon ? "yes" : "no",
             cat_signal_name(cat->signal), cat->w_min, cat->w_max, cat->w_fg, cat->tau_lo,
             cat->tau_hi, cfg.interval_ms);
    cat_trace_row(cat, 0.0, "init");
    return 0;

fail_rmdir:
    cat_remove_groups(cat);
fail_unmount:
    if (cat->mounted_by_us) {
        umount(RESCTRL_ROOT);
        cat->mounted_by_us = false;
    }
    if (cat->trace) {
        fclose(cat->trace);
        cat->trace = NULL;
    }
    cat->enabled = false;
    return rc;
}

/* Add the FG's instruction delta since the previous call to
 * fg_instr_total (the trace's fg_instr column). */
static void cat_account_fg_instr(cat_state_t *cat)
{
    uint64_t now;
    if (!cat_fg_counter(cat, CPU_EVENT_INSTRUCTIONS, &now)) {
        return;
    }
    if (cat->trace_have_instr && now > cat->trace_last_instr) {
        cat->fg_instr_total += now - cat->trace_last_instr;
    }
    cat->trace_last_instr = now;
    cat->trace_have_instr = true;
}

void cat_control_step(pact_context_t *ctx)
{
    cat_state_t *cat = &ctx->cat;
    if (!cat->enabled) {
        return;
    }
    cat->ticks++;
    cat_account_fg_instr(cat);
    bool read_failed = cat->feed.read_failed;
    cat->feed.read_failed = false;
    bool counters = cat->signal == CAT_SIGNAL_MPKI || cat->signal == CAT_SIGNAL_L2LAT ||
                    cat->signal == CAT_SIGNAL_PROBE;
    if (counters && read_failed) {
        cat_skip_tick(cat);
        return;
    }
    switch (cat->signal) {
    case CAT_SIGNAL_MPKI:
        cat_step_mpki(ctx);
        break;
    case CAT_SIGNAL_L2LAT:
        cat_step_l2lat(ctx);
        break;
    case CAT_SIGNAL_PROBE:
        cat_step_probe(ctx);
        break;
    case CAT_SIGNAL_BW:
        cat_step_bw(ctx);
        break;
    case CAT_SIGNAL_STATIC:
    default:
        if ((cat->ticks % 100) == 1) {
            cat_trace_row(cat, 0.0, "hold");
        }
        break;
    }
}

void cat_cleanup(pact_context_t *ctx)
{
    /* The normal teardown ran: the exit handler must not touch the context
     * after it is freed. */
    if (g_cat_atexit_ctx == ctx) {
        g_cat_atexit_ctx = NULL;
    }
    cat_state_t *cat = &ctx->cat;
    if (!cat->enabled) {
        return;
    }
    /* A last row, so a reader can take fg_instr / t from the final line. */
    cat_account_fg_instr(cat);
    cat_trace_row(cat, 0.0, "final");

    /* Restore the full mask before removal, so a failed rmdir does not
     * leave the LLC partitioned. */
    cat_write_group_schemata(cat, cat->fg_grp, cat->full_mask);
    cat_write_group_schemata(cat, cat->bg_grp, cat->full_mask);
    cat_remove_groups(cat);
    if (cat->mounted_by_us) {
        if (umount(RESCTRL_ROOT) != 0) {
            log_warning("cat_cleanup", "umount " RESCTRL_ROOT " failed: %s", strerror(errno));
        }
        cat->mounted_by_us = false;
    }
    if (cat->trace) {
        fclose(cat->trace);
        cat->trace = NULL;
    }
    cat->enabled = false;
    cat->feed.active = false;
    log_info("cat_cleanup",
             "CAT cleanup done (%lu ticks, %lu reallocations, FG "
             "instructions %llu over %.1f s)",
             (unsigned long)cat->ticks, (unsigned long)cat->reallocations,
             (unsigned long long)cat->fg_instr_total, cat_now() - cat->t0);
}
