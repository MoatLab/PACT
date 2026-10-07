/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main
static int scenario, queries;
int numa_move_pages(int pid, unsigned long count, void **pages,
                    const int *nodes, int *status, int flags) {
  assert(pid == 123);
  if (scenario == 99) {
    assert(!nodes && count > 0 && count <= 256 && flags == 0);
    queries++;
    for (unsigned long i = 0; i < count; i++) {
      assert(status[i] == INT_MIN);
      status[i] = 0;
    }
    return 0;
  }

  if (nodes) {
    assert(count == 3 && flags == MPOL_MF_MOVE);
    status[1] = -EBUSY; /* Preserve this kernel-written failure. */
    if (scenario == 4) {
      status[0] = status[2] = 0;
      return 0;
    }
    errno = ENOMEM;
    return scenario == 0 ? 2 : -1;
  }
  queries++;
  assert(count == 2 && flags == 0);
  assert(pages[0] == (void *)4096 && pages[1] == (void *)12288);
  assert(status[0] == INT_MIN && status[1] == INT_MIN);
  if (scenario == 2) {
    status[0] = 0; /* Do not adopt writes after a failed query. */
    errno = EACCES;
    return -1;
  }
  status[0] = 0;
  if (scenario != 3) {
    status[1] = 1;
  }
  return 0;
}
int main(void) {
  set_log_level(LOG_LEVEL_ERROR);
  for (scenario = 0; scenario < 5; scenario++) {
    pact_workload_t wl = {0};
    pact_context_t ctx = {.workload = &wl};
    pac_metadata_t meta[3] = {{.tier = 1, .migrating = true},
                              {.tier = 1, .migrating = true},
                              {.tier = 1, .migrating = true}};
    pac_metadata_t *metas[] = {&meta[0], &meta[1], &meta[2]};
    void *pages[] = {(void *)4096, (void *)8192, (void *)12288};
    int nodes[] = {0, 0, 0}, status[3] = {-1, -1, -1};
    queries = 0;
    mig_dispatch_batch(&ctx, 123, pages, nodes, status, metas, 3);
    assert(queries == (scenario == 4 ? 0 : 1));
    assert(status[1] == -EBUSY && meta[1].tier == 1);
    assert(meta[0].tier == (scenario == 2 ? 1 : 0));
    assert(meta[2].tier == (scenario == 4 ? 0 : 1));
    assert(!meta[0].migrating && !meta[1].migrating && !meta[2].migrating);
    assert(wl.stats.promotion_successes == (scenario == 4   ? 2
                                            : scenario == 2 ? 0
                                                            : 1));
    assert(wl.stats.promotion_failures + wl.stats.promotion_successes == 3);
  }
#ifndef PACT_TEST_BASELINE
  /* Bounded stack batches must also handle configurable large migrations. */
  void *pages[513];
  int nodes[513], status[513];
  for (int i = 0; i < 513; i++) {
    pages[i] = (void *)(uintptr_t)((i + 1) * 4096);
    nodes[i] = 0;
    status[i] = INT_MIN;
  }
  scenario = 99;
  queries = 0;
  recover_migration_status(123, pages, nodes, status, 513);
  assert(queries == 3);
  for (int i = 0; i < 513; i++) {
    assert(status[i] == 0);
  }
  recover_migration_status(123, pages, nodes, status, 513);
  recover_migration_status(123, pages, nodes, status, 0);
  assert(queries == 3);
#endif
}
