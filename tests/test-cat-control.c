/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* LLC way controller without resctrl: signal events and the l2lat counter
 * rule, tau defaults, the way range, the hysteresis law written to
 * temporary schemata files, a split whose last write fails, the
 * controller tick on the counter feed (window sums, skipped tick after a
 * failed read, multiplex parking), and the CLOS groups that exist
 * already. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/cat.c"

static char dir[64];
static char fg[128], bg[128];

static void make_group(char *path, const char *name)
{
    snprintf(path, 128, "%s/%s", dir, name);
    assert(mkdir(path, 0755) == 0);
    char file[192];
    snprintf(file, sizeof(file), "%s/schemata", path);
    FILE *fp = fopen(file, "w");
    assert(fp);
    fclose(fp);
}

static void read_schemata(const char *grp, char *out, size_t len)
{
    char file[192];
    snprintf(file, sizeof(file), "%s/schemata", grp);
    assert(cat_read_str(file, out, len) == 0);
}

static void remove_group(const char *grp)
{
    char file[192];
    snprintf(file, sizeof(file), "%s/schemata", grp);
    unlink(file);
    rmdir(grp);
}

/* One controller tick after the feed gained these window sums. */
static void tick(pact_context_t *ctx, uint64_t instr, uint64_t misses, uint64_t enabled,
                 uint64_t running)
{
    cat_feed_t *feed = &ctx->cat.feed;
    feed->count[CPU_EVENT_INSTRUCTIONS] += instr;
    feed->count[CPU_EVENT_LLC_MISS_ALL] += misses;
    feed->time_enabled += enabled;
    feed->time_running += running;
    cat_control_step(ctx);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);

    /* Signal events. l2lat needs more than 4 general-purpose counters. */
    uint32_t ev;
    assert(cat_signal_events(CAT_SIGNAL_MPKI, false, 4, &ev) == 0);
    assert(ev == ((1u << CPU_EVENT_INSTRUCTIONS) | (1u << CPU_EVENT_LLC_MISS_ALL)));
    assert(cat_signal_events(CAT_SIGNAL_L2LAT, false, 4, &ev) < 0);
    assert(cat_signal_events(CAT_SIGNAL_L2LAT, false, 8, &ev) == 0);
    assert(ev == ((1u << CPU_EVENT_INSTRUCTIONS) | (1u << CPU_EVENT_OFFCORE_DD_OUT) |
                  (1u << CPU_EVENT_OFFCORE_DD)));
    assert(cat_signal_events(CAT_SIGNAL_PROBE, false, 4, &ev) == 0);
    assert(ev == ((1u << CPU_EVENT_INSTRUCTIONS) | (1u << CPU_EVENT_CYCLES)));
    assert(cat_signal_events(CAT_SIGNAL_STATIC, false, 4, &ev) == 0 && ev == 0);
    assert(cat_signal_events(CAT_SIGNAL_BW, true, 4, &ev) == 0);
    assert(ev == (1u << CPU_EVENT_INSTRUCTIONS));

    /* Tau defaults follow the signal's unit; explicit values stay. */
    cat_config_t cfg;
    cat_default_config(&cfg);
    cat_resolve_taus(&cfg);
    assert(cfg.tau_lo == 10.0 && cfg.tau_hi == 25.0 && cfg.interval_ms == 100);
    cat_default_config(&cfg);
    cfg.signal = CAT_SIGNAL_L2LAT;
    cfg.tau_hi = 150.0;
    cat_resolve_taus(&cfg);
    assert(cfg.tau_lo == 110.0 && cfg.tau_hi == 150.0);

    /* Way range: (ways + 2) / 4 .. 3 * ways / 4 by default. */
    cat_state_t cat;
    memset(&cat, 0, sizeof(cat));
    cat_default_config(&cat.cfg);
    cat.min_cbm_bits = 1;
    cat.num_ways = 11;
    assert(cat_set_range(&cat) == 0 && cat.w_min == 3 && cat.w_max == 8);
    cat.num_ways = 20;
    assert(cat_set_range(&cat) == 0 && cat.w_min == 5 && cat.w_max == 15);
    cat.cfg.wmax = 20; /* the BG keeps at least min_cbm_bits ways */
    assert(cat_set_range(&cat) == -2);
    cat.cfg.wmin = 9;
    cat.cfg.wmax = 8;
    assert(cat_set_range(&cat) == -2);
    cat.cfg.wmin = -1;
    cat.cfg.wmax = 4; /* the automatic W_min (5) yields to the explicit W_max */
    assert(cat_set_range(&cat) == 0 && cat.w_min == 4 && cat.w_max == 4);

    /* Hysteresis law on temporary group directories (11 ways, 3..8). */
    snprintf(dir, sizeof(dir), "/tmp/pact-cat-XXXXXX");
    assert(mkdtemp(dir));
    make_group(fg, "fg");
    make_group(bg, "bg");
    pact_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pact_workload_t wl = {.index = 0};
    ctx.workloads = &wl;
    ctx.nr_workloads = 1;
    ctx.running = true;
    cat_state_t *c = &ctx.cat;
    cat_default_config(&c->cfg);
    cat_resolve_taus(&c->cfg);
    c->enabled = true;
    c->signal = CAT_SIGNAL_MPKI;
    c->num_ways = 11;
    c->full_mask = 0x7ff;
    c->min_cbm_bits = 1;
    c->nr_domains = 2;
    c->domain_ids[0] = 0;
    c->domain_ids[1] = 1;
    c->w_min = 3;
    c->w_max = 8;
    c->w_fg = 8;
    c->tau_lo = 10;
    c->tau_hi = 25;
    snprintf(c->fg_grp, sizeof(c->fg_grp), "%s", fg);
    snprintf(c->bg_grp, sizeof(c->bg_grp), "%s", bg);
    char text[256];
    assert(strcmp(cat_hysteresis_law(c, 30), "hold") == 0 && c->w_fg == 8);
    assert(strcmp(cat_hysteresis_law(c, 5), "shed") == 0 && c->w_fg == 7);
    read_schemata(fg, text, sizeof(text));
    assert(strcmp(text, "L3:0=7f0;1=7f0\n") == 0); /* top 7 of 11 ways */
    read_schemata(bg, text, sizeof(text));
    assert(strcmp(text, "L3:0=f;1=f\n") == 0);
    assert(strcmp(cat_hysteresis_law(c, 15), "hold") == 0 && c->w_fg == 7);
    assert(strcmp(cat_hysteresis_law(c, 30), "attack") == 0 && c->w_fg == 8);

    /* The BG write fails after the FG write. The FG gets the mask of the
     * current split back, and W_fg stays. */
    snprintf(c->bg_grp, sizeof(c->bg_grp), "%s/missing", dir);
    uint64_t reallocations = c->reallocations;
    assert(!cat_set_ways(c, 7, "shed") && c->w_fg == 8 && c->reallocations == reallocations);
    read_schemata(fg, text, sizeof(text));
    assert(strcmp(text, "L3:0=7f8;1=7f8\n") == 0);
    /* A shed that cannot program the split holds. */
    assert(strcmp(cat_hysteresis_law(c, 5), "hold-gated") == 0 && c->w_fg == 8);
    snprintf(c->bg_grp, sizeof(c->bg_grp), "%s", bg);

    /* Controller ticks on the feed: mpki = 1000 * misses / instructions
     * over the tick. Tick 1 takes the snapshot. */
    c->feed.active = true;
    c->feed.have[CPU_EVENT_INSTRUCTIONS] = true;
    c->feed.have[CPU_EVENT_LLC_MISS_ALL] = true;
    tick(&ctx, 1000000, 100, 100, 100);
    assert(c->have_snapshot && c->w_fg == 8);
    tick(&ctx, 1000000, 1000, 100, 100); /* mpki 1: release one way */
    assert(c->w_fg == 7);
    /* A failed FG read skips the tick: no decision, new base. */
    c->feed.read_failed = true;
    tick(&ctx, 1000000, 100000, 100, 100); /* mpki 100 would restore W_max */
    assert(c->w_fg == 7 && !c->feed.read_failed);
    tick(&ctx, 1000000, 1000, 100, 100);
    assert(c->w_fg == 6);
    /* Multiplexed groups (running 50% of enabled): park at W_max. */
    tick(&ctx, 1000000, 1000, 100, 50);
    assert(c->multiplexed && c->w_fg == 8);
    /* A tick without a measurement (no new window) keeps it parked. */
    tick(&ctx, 0, 0, 0, 0);
    assert(c->multiplexed && c->w_fg == 8);
    /* A measured whole tick resumes the law. */
    tick(&ctx, 1000000, 1000, 100, 100);
    assert(!c->multiplexed && c->w_fg == 7);
    /* Below the idle gate (1e6 instructions/s over 100 ms): hold. */
    tick(&ctx, 50000, 50, 100, 100);
    assert(c->w_fg == 7);

    /* Without the counters on every FG CPU the controller holds. */
    c->feed.have[CPU_EVENT_LLC_MISS_ALL] = false;
    tick(&ctx, 1000000, 1000, 100, 100);
    assert(c->w_fg == 7 && c->warned_no_signal);

    /* A group that exists is not taken over, and the cleanup removes only
     * the groups that this run created. */
    char made[160];
    snprintf(made, sizeof(made), "%s/made", dir);
    bool created = false;
    assert(cat_make_group(fg, &created) == -2 && !created);
    assert(cat_make_group(made, &created) == 0 && created);
    snprintf(c->bg_grp, sizeof(c->bg_grp), "%s", made);
    c->bg_created = true;
    c->fg_created = false;
    cat_remove_groups(c);
    assert(access(made, F_OK) != 0 && access(fg, F_OK) == 0);
    assert(!c->bg_created && !c->fg_created);

    remove_group(fg);
    remove_group(bg);
    rmdir(dir);
    return 0;
}
