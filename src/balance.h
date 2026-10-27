/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* balance.h: migration balance check. */

#ifndef PACT_BALANCE_H
#define PACT_BALANCE_H

#include <stdint.h>
#include "pact.h"

/* Read a workload cgroup's pgdemote count (memory.stat): pages of the
 * cgroup demoted to the lower tier since the cgroup was created. Returns 0,
 * or -1 when the count is unreadable. */
int read_cgroup_demotions(const char *cgroup_path, uint64_t *value);

/* Read this run's demotions of every live workload's cgroup into its
 * statistics, in both modes. Called ~1 Hz from the migration thread. */
void check_migration_balance(pact_context_t *pact);

#endif /* PACT_BALANCE_H */
