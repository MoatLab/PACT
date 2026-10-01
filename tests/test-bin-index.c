/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <float.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

static void check_admission(uint64_t score, double width, bool expected) {
  double samples[100] = {0};
  reservoir_t reservoir = {.samples = samples, .capacity = 100};
  binning_state_t bin = {.bin_count = 20, .bin_width = width};
  pact_workload_t wl = {.reservoir = &reservoir, .binning = &bin};
  wl.pac_table = pac_table_init();
  wl.migration_ring = ring_buffer_migration_entry_create(8);
  assert(wl.migration_ring);
  pact_context_t ctx = {.workload = &wl, .cooling_alpha = 1};
  init_pac_metadata_pool(&ctx);
  update_pac_entry(&ctx, 4096, score, 1, 123);
  assert(ring_buffer_migration_entry_size(wl.migration_ring) ==
         (size_t)expected);
  khint_t k = pac_table_get(wl.pac_table, 4096);
  assert(k != kh_end(wl.pac_table));
  assert(kh_val(wl.pac_table, k)->pac_value == score);
  ring_buffer_migration_entry_destroy(wl.migration_ring);
  pac_table_destroy(wl.pac_table);
  pool_destroy(ctx.pac_metadata_pool);
}

int main(void) {
  set_log_level(LOG_LEVEL_ERROR);
  /* Finite positive CLI widths can produce an unrepresentable bin index. */
  check_admission(1, 1e-300, true);
  check_admission(1, DBL_MIN / 2, true);
  check_admission(UINT64_MAX, 1, true);
  check_admission(UINT64_MAX, 0.5, true);
  check_admission(0, DBL_MIN / 2, false);
  check_admission(UINT64_MAX, 1e300, false);
  /* Preserve normal threshold behavior on both sides of the top bin. */
  check_admission(18999, 1000, false);
  check_admission(19000, 1000, true);
  check_admission(19001, 1000, true);
  check_admission(9, 0.5, false);
  check_admission(10, 0.5, true);
  puts("bin index: extreme widths and ordinary promotion boundaries passed");
  return 0;
}
