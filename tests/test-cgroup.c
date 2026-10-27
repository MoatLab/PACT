/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* Workload cgroups with temporary directories: path resolution, the
 * enabled/disabled switches, the fast-tier cap check, memory.stat sums, and
 * the per-workload demotion counters of both modes, which read memory.stat
 * and do not write memory.demotion. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/balance.h"
#include "../src/cgroup.h"
#include "../src/error.h"
#include "../src/pact.h"

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    assert(fp);
    fputs(text, fp);
    fclose(fp);
}

static void read_file(const char *dir, const char *name, char *out, size_t len)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "r");
    assert(fp);
    size_t n = fread(out, 1, len - 1, fp);
    out[n] = '\0';
    fclose(fp);
}

static void remove_files(const char *dir)
{
    static const char *const names[] = {"memory.max_at_node0", "memory.stat", "memory.demotion",
                                        "memory.mmctl_enabled"};
    for (size_t i = 0; i < 4; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    char out[256];

    /* A relative word resolves under /sys/fs/cgroup; an absolute one stays. */
    assert(pact_cgroup_path("pact/bc", out, sizeof(out)) == 0);
    assert(strcmp(out, "/sys/fs/cgroup/pact/bc") == 0);
    assert(pact_cgroup_path("/sys/fs/cgroup/x", out, sizeof(out)) == 0);
    assert(strcmp(out, "/sys/fs/cgroup/x") == 0);
    assert(pact_cgroup_path("abcdefgh", out, 10) < 0);

    char dirs[2][64];
    for (int d = 0; d < 2; d++) {
        snprintf(dirs[d], sizeof(dirs[d]), "/tmp/pact-cgroup-XXXXXX");
        assert(mkdtemp(dirs[d]));
    }

    /* The two switches that make the kernel enforce the cap: the bracket
     * marks the current value, and any other text is unreadable. */
    assert(pact_cgroup_switch(dirs[0], "memory.mmctl_enabled") == -1); /* no file */
    write_file(dirs[0], "memory.mmctl_enabled", "[enabled] disabled\n");
    assert(pact_cgroup_switch(dirs[0], "memory.mmctl_enabled") == 1);
    write_file(dirs[0], "memory.mmctl_enabled", "enabled [disabled]\n");
    assert(pact_cgroup_switch(dirs[0], "memory.mmctl_enabled") == 0);
    write_file(dirs[0], "memory.mmctl_enabled", "enabled\n");
    assert(pact_cgroup_switch(dirs[0], "memory.mmctl_enabled") == -1);

    /* The cap must be readable (-1 otherwise) and finite (-2 otherwise). */
    uint64_t cap = 0;
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == -1); /* no file */
    write_file(dirs[0], "memory.max_at_node0", "max\n");
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == -2);
    write_file(dirs[0], "memory.max_at_node0", "12x\n");
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == -2);
    write_file(dirs[0], "memory.max_at_node0", "1073741824\n");
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == 0 && cap == 1073741824ULL);

    /* The kernel's page-counter maximum (LONG_MAX / page size pages) in
     * bytes, for example 9223372036854771712 with 4 KiB pages, is no finite
     * cap. One page less is a finite cap. */
    long page = sysconf(_SC_PAGESIZE);
    unsigned long long unlimited = (unsigned long long)(LONG_MAX / page) * page;
    char text[64];
    snprintf(text, sizeof(text), "%llu\n", unlimited);
    write_file(dirs[0], "memory.max_at_node0", text);
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == -2);
    if (page == 4096) {
        assert(unlimited == 9223372036854771712ULL);
    }
    write_file(dirs[0], "memory.max_at_node0", "18446744073709551615\n");
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == -2);
    snprintf(text, sizeof(text), "%llu\n", unlimited - page);
    write_file(dirs[0], "memory.max_at_node0", text);
    assert(pact_cgroup_node0_cap(dirs[0], &cap) == 0 && cap == unlimited - page);

    /* memory.stat sums; a file without any key is an error. */
    static const char *const keys[] = {"pgsteal_kswapd", "pgsteal_direct"};
    uint64_t sum = 0;
    write_file(dirs[0], "memory.stat", "anon 5\npgsteal_kswapd 7\npgsteal_direct 3\n");
    assert(pact_cgroup_stat_sum(dirs[0], keys, 2, &sum) == 0 && sum == 10);
    write_file(dirs[0], "memory.stat", "anon 5\n");
    assert(pact_cgroup_stat_sum(dirs[0], keys, 2, &sum) < 0);
    uint64_t demoted = 0;
    write_file(dirs[0], "memory.stat", "pgdemote_kswapd 4\npgdemote 9\n");
    assert(read_cgroup_demotions(dirs[0], &demoted) == 0 && demoted == 9);

    /* Demotion counters per workload, from the startup baseline: workload 0
     * demoted 5 pages and workload 1 demoted 20. The balance check reads
     * them and does not write memory.demotion, whatever the promotion
     * count. */
    pact_workload_t wl[2];
    memset(wl, 0, sizeof(wl));
    for (int w = 0; w < 2; w++) {
        wl[w].index = w;
        snprintf(wl[w].cgroup_path, sizeof(wl[w].cgroup_path), "%s", dirs[w]);
        wl[w].demotion_baseline = 100;
        atomic_store(&wl[w].stats.promotion_successes, 10);
        write_file(dirs[w], "memory.demotion", "");
    }
    write_file(dirs[0], "memory.stat", "pgdemote 105\n");
    write_file(dirs[1], "memory.stat", "pgdemote 120\n");
    pact_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.workloads = wl;
    ctx.nr_workloads = 2;
    check_migration_balance(&ctx);
    for (int w = 0; w < 2; w++) {
        read_file(dirs[w], "memory.demotion", out, sizeof(out));
        assert(out[0] == '\0');
    }
    assert(wl[0].stats.demotion_successes == 5 && wl[1].stats.demotion_successes == 20);

    /* An exited workload is skipped: its counters stay. */
    atomic_store(&wl[1].exited, true);
    write_file(dirs[1], "memory.stat", "pgdemote 150\n");
    check_migration_balance(&ctx);
    assert(wl[1].stats.demotion_successes == 20);
    atomic_store(&wl[1].exited, false);

    /* Tierce mode reads the counters the same way. */
    ctx.mode = PACT_MODE_TIERCE;
    write_file(dirs[0], "memory.stat", "pgdemote 140\n");
    check_migration_balance(&ctx);
    read_file(dirs[0], "memory.demotion", out, sizeof(out));
    assert(out[0] == '\0' && wl[0].stats.demotion_successes == 40);

    remove_files(dirs[0]);
    remove_files(dirs[1]);
    return 0;
}
