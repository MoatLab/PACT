/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* validate.c: CLI configuration sanity checks. */

#include <stdio.h>
#include <string.h>

#include "utils.h"
#include "validate.h"

/* Check one --workload argument: the CGROUP word is required, and a CPUS
 * word other than "auto" must be a valid CPU list. */
static int validate_workload(const pact_config_t *config, int index)
{
    const pact_workload_config_t *w = &config->workloads[index];
    if (w->cgroup[0] == '\0') {
        fprintf(stderr,
                "Error: --workload %d (PID %d) needs PID CPUS NAME CGROUP; CGROUP (the "
                "workload's own cgroup with a fast-tier cap) is required.\n",
                index, (int)w->pid);
        return -1;
    }
    bool cpus_given = w->cpus[0] != '\0' && strcmp(w->cpus, PACT_CPUS_AUTO) != 0;
    if (config->mode == PACT_MODE_TIERCE && !cpus_given) {
        fprintf(stderr,
                "Error: --workload %d (PID %d): Tierce mode needs a CPU list (CPUS) for "
                "every workload\n",
                index, (int)w->pid);
        return -1;
    }
    if (cpus_given) {
        uint64_t mask;
        if (pact_parse_cpu_list(w->cpus, &mask) < 0) {
            fprintf(stderr,
                    "Error: --workload %d (PID %d): invalid CPU list '%s' (use a list such as "
                    "2-5,8 with CPUs 0-63, or %s)\n",
                    index, (int)w->pid, w->cpus, PACT_CPUS_AUTO);
            return -1;
        }
    }
    for (int j = 0; j < index; j++) {
        if (config->workloads[j].pid == w->pid) {
            fprintf(stderr, "Error: PID %d is given in more than one --workload\n", (int)w->pid);
            return -1;
        }
    }
    return 0;
}

/* Tierce: the workloads' CPU lists must not overlap, so every CPU's
 * counters and samples belong to one domain. */
static int validate_cpu_overlap(const pact_config_t *config)
{
    uint64_t used = 0;
    for (int i = 0; i < config->nr_workloads; i++) {
        uint64_t mask = 0;
        if (pact_parse_cpu_list(config->workloads[i].cpus, &mask) < 0) {
            return -1; /* validate_workload reported it */
        }
        if (used & mask) {
            fprintf(stderr,
                    "Error: --workload %d (PID %d): CPU list '%s' overlaps another workload; "
                    "Tierce mode needs disjoint CPU lists\n",
                    i, (int)config->workloads[i].pid, config->workloads[i].cpus);
            return -1;
        }
        used |= mask;
    }
    return 0;
}

int pact_validate_configuration(const pact_config_t *config)
{
    if (config->nr_workloads <= 0) {
        fprintf(stderr, "Error: --workload PID CPUS NAME CGROUP is required.\n");
        return -1;
    }
    for (int i = 0; i < config->nr_workloads; i++) {
        if (validate_workload(config, i) < 0) {
            return -1;
        }
    }
    if (config->mode == PACT_MODE_TIERCE && validate_cpu_overlap(config) < 0) {
        return -1;
    }

    if (config->sampling_interval_ms < 5) {
        fprintf(
            stderr,
            "Warning: --sampling-interval %u ms is very aggressive and may cause high overhead\n",
            config->sampling_interval_ms);
    }

    return 0;
}
