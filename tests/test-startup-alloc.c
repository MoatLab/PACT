/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

static void *live[64];
static unsigned calls, fail_at;
static bool armed;
void *__real_calloc(size_t n, size_t size);
void __real_free(void *p);
void *__wrap_calloc(size_t n, size_t size)
{
    if (armed && ++calls == fail_at) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = __real_calloc(n, size);
    if (armed && p) {
        for (unsigned i = 0; i < 64; i++) {
            if (!live[i]) {
                live[i] = p;
                return p;
            }
        }
        abort();
    }
    return p;
}
void __wrap_free(void *p)
{
    if (p) {
        for (unsigned i = 0; i < 64; i++) {
            if (live[i] == p) {
                live[i] = NULL;
            }
        }
    }
    __real_free(p);
}
/* Stable affinity fixture isolates startup allocation from host CPU numbering.
 */
int __wrap_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *mask)
{
    (void)pid;
    (void)size;
    CPU_ZERO(mask);
    CPU_SET(0, mask);
    return 0;
}
/* The per-CPU state fills no entry when its calloc fails: these stand in
 * for perf.c, pmu.c and pmu-platform.c, which are not linked. */
pmu_platform_t g_pmu_platform;
void init_per_cpu_state(per_cpu_state_t *cs)
{
    (void)cs;
}
void close_cpu_counting_group(per_cpu_state_t *cs)
{
    (void)cs;
}

/* Mode 1: a failed calloc of the per-CPU state is an error, and the
 * teardown takes the NULL array. */
static int check_cpu_states(void)
{
    int cpus[2] = {0, 1};
    pact_context_t ctx = {0};
    ctx.all_cpus = cpus;
    ctx.nr_all_cpus = 2;
    armed = true;
    int rc = init_cpu_states(&ctx);
    assert((fail_at == 1) == (rc < 0) && (rc < 0) == (ctx.cpu_states == NULL));
    destroy_per_cpu_pebs(&ctx);
    armed = false;
    assert(!ctx.cpu_states);
    printf("CPU_STATES_RESULT rc=%d\n", rc);
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    fail_at = strtoul(argv[1], NULL, 10);
    pact_config_t config;
    pact_init_config(&config);
    config.nr_workloads = 1;
    config.workloads[0].pid = getpid();
    int mode = atoi(argv[2]);
    assert(mode == 0 || mode == 1);
    set_log_level(LOG_LEVEL_ERROR);
    if (mode == 1) {
        return check_cpu_states();
    }
    pact_context_t ctx = {0};
    armed = true;
    int rc = initialize_workloads(&ctx, &config);
    if (!rc) {
        assert(ctx.workloads && ctx.nr_domains == 1 && ctx.domains[0].table &&
               ctx.domains[0].reservoir && ctx.domains[0].binning);

        destroy_domains(&ctx);
        free(ctx.workloads[0].target_cpus);
        free(ctx.workloads);
        free(ctx.all_cpus);
    }
    armed = false;
    unsigned remaining = 0;
    for (unsigned i = 0; i < 64; i++) {
        remaining += live[i] != NULL;
    }
    printf("STARTUP_RESULT calls=%u rc=%d remaining=%u\n", calls, rc, remaining);
    assert(remaining == 0);
    assert((fail_at == 0) == (rc == 0));
    return 0;
}
