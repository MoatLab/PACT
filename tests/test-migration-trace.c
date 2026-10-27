/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* --trace-migration: the header of each mode (both write per-cgroup
 * kernel demotions), totals and rates per row, kernel demotions counted
 * from the run's start, and the last value kept for an unreadable count. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/error.h"
#include "../src/pact.h"
#include "../src/trace.h"
#include "../src/tsc.h"
#include "../src/utils.h"

static char dir[64];

static void set_pgdemote(unsigned long value)
{
    char path[128], text[64];
    snprintf(path, sizeof(path), "%s/memory.stat", dir);
    snprintf(text, sizeof(text), "pgdemote %lu\npgsteal_kswapd 0\n", value);
    FILE *fp = fopen(path, "w");
    assert(fp);
    fputs(text, fp);
    fclose(fp);
}

static void read_lines(const char *path, char lines[][1024], int max)
{
    FILE *fp = fopen(path, "r");
    assert(fp);
    for (int i = 0; i < max; i++) {
        lines[i][0] = '\0';
        if (!fgets(lines[i], 1024, fp)) {
            break;
        }
        lines[i][strcspn(lines[i], "\n")] = '\0';
    }
    fclose(fp);
}

static void run(pact_mode_t mode, const char *path, bool unreadable)
{
    pact_workload_t wl[2];
    memset(wl, 0, sizeof(wl));
    for (int w = 0; w < 2; w++) {
        wl[w].index = w;
        snprintf(wl[w].cgroup_path, sizeof(wl[w].cgroup_path), "%s", dir);
        wl[w].demotion_baseline = 1000; /* pgdemote at start */
    }
    pact_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mode = mode;
    ctx.workloads = wl;
    ctx.nr_workloads = 2;
    ctx.tsc_freq_hz = pact_detect_tsc_frequency();
    ctx.start_tsc = rdtsc();
    set_pgdemote(1000);
    assert(migration_trace_open(&ctx, path) == 0);
    assert(ctx.migration_trace.next_tsc != 0);
    usleep(20000);
    atomic_store(&wl[0].stats.promotion_successes, 30);
    atomic_store(&wl[1].stats.promotion_successes, 10);
    set_pgdemote(1100);
    migration_trace_row(&ctx);
    if (unreadable) {
        /* A second row with memory.stat gone repeats the last count. */
        char stat[128];
        snprintf(stat, sizeof(stat), "%s/memory.stat", dir);
        unlink(stat);
        usleep(20000);
        migration_trace_row(&ctx);
        assert(ctx.migration_trace.kernel_read_failures == 2);
    }
    migration_trace_close(&ctx);
    assert(ctx.migration_trace.file == NULL && ctx.migration_trace.next_tsc == 0);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    snprintf(dir, sizeof(dir), "/tmp/pact-trace-XXXXXX");
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof(path), "%s/trace.csv", dir);
    char lines[4][1024];
    double t, rate;
    unsigned long promotions, demotions, kernel;

    pact_mode_t modes[2] = {PACT_MODE_PACT, PACT_MODE_TIERCE};
    for (int m = 0; m < 2; m++) {
        run(modes[m], path, modes[m] == PACT_MODE_PACT);
        read_lines(path, lines, 4);
        assert(strstr(lines[0], "elapsed_sec,total_pact_promotions,pact_promotion_speed"));
        assert(strstr(lines[0], ",wl0_kernel_demotions,wl0_kernel_demotion_speed"));
        assert(strstr(lines[0], ",wl1_kernel_demotions,wl1_kernel_demotion_speed"));
        int commas_header = 0, commas_row = 0;
        for (char *p = lines[0]; *p; p++) {
            commas_header += *p == ',';
        }
        for (char *p = lines[1]; *p; p++) {
            commas_row += *p == ',';
        }
        assert(commas_header == 6 + 2 * 6 && commas_row == commas_header);
        /* totals: promotions 40, runtime demotions 0, kernel demotions 2 x 100 */
        assert(sscanf(lines[1], "%lf,%lu,%lf,%lu,%*f,%lu", &t, &promotions, &rate, &demotions,
                      &kernel) == 5);
        assert(promotions == 40 && demotions == 0 && kernel == 200 && rate > 0);
        assert(strstr(lines[1], ",30,") && strstr(lines[1], ",100,"));
        if (modes[m] == PACT_MODE_PACT) {
            /* The unreadable second row keeps the totals, at rate 0. */
            assert(sscanf(lines[2], "%lf,%lu,%lf,%lu,%*f,%lu,%lf", &t, &promotions, &rate,
                          &demotions, &kernel, &rate) == 6);
            assert(promotions == 40 && kernel == 200 && rate == 0.0);
            assert(lines[3][0] == '\0');
        } else {
            assert(lines[2][0] == '\0');
        }
    }

    unlink(path);
    char stat[128];
    snprintf(stat, sizeof(stat), "%s/memory.stat", dir);
    unlink(stat);
    rmdir(dir);
    return 0;
}
