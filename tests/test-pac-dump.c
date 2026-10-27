/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* DIAG build: the PAC-table dump (PACT top-bin bar, Tierce 0.90 bar, rows
 * with PAC above 0, hash thinning) and --no-promote-wl in Tierce admission. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/admission.h"
#include "../src/diag.h"
#include "../src/error.h"
#include "../src/pact.h"
#include "../src/utils.h"

static pac_metadata_t *put(pac_domain_t *domain, int workload, uint64_t page, uint64_t pac,
                           int tier)
{
    pac_metadata_t *meta = calloc(1, sizeof(*meta));
    assert(meta);
    meta->workload = (uint8_t)workload;
    meta->page_addr = page;
    meta->pac_value = pac;
    meta->tier = tier;
    int ret;
    khint_t k = pac_table_put(domain->table, pac_key(workload, page), &ret);
    assert(ret >= 0);
    kh_val(domain->table, k) = meta;
    return meta;
}

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "r");
    assert(fp);
    static char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, fp);
    text[n] = '\0';
    fclose(fp);
    return text;
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    assert(pac_dump_subsample_mask(0) == 0 && pac_dump_subsample_mask(PAC_DUMP_MAX_ROWS) == 0);
    assert(pac_dump_subsample_mask(PAC_DUMP_MAX_ROWS + 1) == 1);
    assert(pac_dump_subsample_mask(4 * PAC_DUMP_MAX_ROWS) == 3);

    char path[64];
    snprintf(path, sizeof(path), "/tmp/pact-dump-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    /* PACT: one table for both workloads, bar = (20 - 1) * 50 = 950. */
    pac_domain_t one = {0};
    one.table = pac_table_init();
    binning_state_t bin = {.bin_width = 50, .bin_count = 20};
    one.binning = &bin;
    pac_metadata_t *m[4];
    m[0] = put(&one, 0, 0x1000, 0, 1); /* PAC 0: not written */
    m[1] = put(&one, 0, 0x2000, 100, 1);
    m[2] = put(&one, 1, 0x2000, 2000, 0); /* same address, other workload */
    pact_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.domains = &one;
    ctx.nr_domains = 1;
    ctx.tsc_freq_hz = 1000000000ULL;
    ctx.start_tsc = rdtsc();
    assert(pac_dump_open(&ctx, path, 30) == 0);
    pac_dump_snapshot(&ctx);
    pac_dump_close(&ctx);
    char *text = slurp(path);
    assert(strstr(text, "mode pact, 1 tables"));
    assert(strstr(text, "# table0_admission_bar=950.0000"));
    assert(strstr(text, "table0_bar_mode=topbin table0_table_entries=3 table0_subsample=1/1"));
    assert(strstr(text, ",0,0x2000,100,0,1\n") && strstr(text, ",1,0x2000,2000,0,0\n"));
    assert(!strstr(text, "0x1000"));

    /* Tierce: two tables with their own bars. */
    pac_domain_t two[2];
    memset(two, 0, sizeof(two));
    pact_workload_t wl[2] = {{.index = 0, .domain = 0}, {.index = 1, .domain = 1}};
    for (int d = 0; d < 2; d++) {
        two[d].table = pac_table_init();
        two[d].pages = page_reservoir_create(TIERCE_RESERVOIR_PAGES);
        two[d].migration_ring = ring_buffer_migration_entry_create(16);
    }
    two[0].bar = 123.5;
    m[3] = put(&two[1], 1, 0x5000, 77, 1);
    page_reservoir_add(two[1].pages, m[3]);
    ctx.mode = PACT_MODE_TIERCE;
    ctx.domains = two;
    ctx.nr_domains = 2;
    ctx.workloads = wl;
    ctx.nr_workloads = 2;
    ctx.pac_epoch = 1;
    assert(pac_dump_open(&ctx, path, 5) == 0);
    pac_dump_snapshot(&ctx);
    pac_dump_close(&ctx);
    text = slurp(path);
    assert(strstr(text, "mode tierce, 2 tables") && strstr(text, "interval_sec=5"));
    assert(strstr(text, "# table0_admission_bar=123.5000"));
    assert(strstr(text, "table1_bar_mode=quantile table1_table_entries=1"));
    assert(strstr(text, "table1_reservoir=1") && strstr(text, ",1,0x5000,77,0,1\n"));

    /* --no-promote-wl 1: workload 1's admitted page stays out of its ring. */
    ctx.no_promote_mask = 1u << 1;
    epoch_list_page(&two[1], m[3], ctx.pac_epoch);
    two[1].epoch_updates = 1;
    assert(end_pac_epoch(&ctx) == 0);
    assert(ring_buffer_migration_entry_size(two[1].migration_ring) == 0 && !m[3]->migrating);
    ctx.no_promote_mask = 0;
    epoch_list_page(&two[1], m[3], ctx.pac_epoch);
    two[1].epoch_updates = 1;
    assert(end_pac_epoch(&ctx) == 1);

    for (int i = 0; i < 4; i++) {
        free(m[i]);
    }
    pac_table_destroy(one.table);
    for (int d = 0; d < 2; d++) {
        pac_table_destroy(two[d].table);
        page_reservoir_destroy(two[d].pages);
        free(two[d].epoch_pages);
        ring_buffer_migration_entry_destroy(two[d].migration_ring);
    }
    unlink(path);
    return 0;
}
