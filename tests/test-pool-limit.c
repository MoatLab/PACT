/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

/* Unused CLI actions are isolated from this metadata-limit fixture. */
void pact_print_usage(const char *name)
{
    (void)name;
}
void pact_print_version(void) {}
void pact_signal_set_crash_marker_path(const char *path)
{
    (void)path;
}

static void check_limit(const char *text, size_t expected)
{
    pact_config_t config;
    pact_init_config(&config);
    config.target_pid = getpid();
    char *args[] = {"pact", "--pac-pool-max", (char *)text};
    assert(pact_parse_command_line_args(3, args, &config) == 0);
    assert(config.pac_pool_max == expected);
    pact_workload_t workload = {0};
    pact_context_t context = {.max_pac_entries = config.pac_pool_max, .workload = &workload};
    init_pac_metadata_pool(&context);
    assert(context.pac_metadata_pool && context.max_pac_entries == expected);
    pac_metadata_t *first = alloc_pac_metadata(&context);
    pac_metadata_t *second = alloc_pac_metadata(&context);
    assert(first);
    assert((second != NULL) == (expected != 1));
    assert(workload.stats.pool_alloc_skipped == (expected == 1));
    free_pac_metadata(&context, first);
    if (second) {
        free_pac_metadata(&context, second);
    }
    pool_destroy(context.pac_metadata_pool);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    pact_config_t config;
    pact_init_config(&config);
    config.target_pid = getpid();
    assert(config.pac_pool_max == PACT_DEFAULT_PAC_POOL_MAX);
    check_limit("0", 0);
    check_limit("1", 1);
    check_limit("2097152", 2097152);
    check_limit("4194304", 4194304);
    char maximum[32];
    snprintf(maximum, sizeof(maximum), "%zu", SIZE_MAX);
    check_limit(maximum, SIZE_MAX);
    const char *invalid[] = {"",
                             "-1",
                             "+1",
                             " 1",
                             "1 ",
                             "1x",
                             "1.5",
                             "abc",
                             "18446744073709551616",
                             "99999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        pact_init_config(&config);
        config.target_pid = getpid();
        char *args[] = {"pact", "--pac-pool-max", (char *)invalid[i]};
        assert(pact_parse_command_line_args(3, args, &config) == -1);
        assert(config.pac_pool_max == PACT_DEFAULT_PAC_POOL_MAX);
    }
    char *missing[] = {"pact", "--pac-pool-max"};
    assert(pact_parse_command_line_args(2, missing, &config) == -1);
    puts("PASS: metadata limit defaults, unlimited mode, explicit caps and "
         "invalid arguments");
}
