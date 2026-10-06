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
int main(int argc, char **argv)
{
    assert(argc == 3);
    fail_at = strtoul(argv[1], NULL, 10);
    pact_config_t config;
    pact_init_config(&config);
    config.target_pid = getpid();
    assert(atoi(argv[2]) == 0);
    pact_context_t ctx = {0};
    set_log_level(LOG_LEVEL_ERROR);
    armed = true;
    int rc = initialize_workloads(&ctx, &config);
    if (!rc) {
        assert(ctx.workload && ctx.workload->pac_table && ctx.workload->reservoir &&
               ctx.workload->binning);

        destroy_per_workload_data(&ctx);
        free(ctx.workload->target_cpus);
        free(ctx.workload);
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
