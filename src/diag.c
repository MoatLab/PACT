/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* diag.c: the PAC-table snapshot dump of a `make DIAG=1` build (see
 * diag.h). The Makefile compiles this file only in that build. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "error.h"
#include "pact.h"
#include "tsc.h"

#define PAC_DUMP_STDIO_BUF_BYTES (4u * 1024u * 1024u)

/* splitmix64 finalizer over the page frame number, so neighbouring pages
 * land in unrelated buckets. */
static inline uint64_t pac_dump_hash(uint64_t key)
{
    uint64_t x = key >> 12;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint32_t pac_dump_subsample_mask(size_t n)
{
    unsigned shift = 0;
    while (shift < 20 && (n >> shift) > PAC_DUMP_MAX_ROWS) {
        shift++;
    }
    return (1u << shift) - 1u;
}

int pac_dump_open(pact_context_t *ctx, const char *path, uint32_t interval_s)
{
    pac_dump_t *dump = &ctx->pac_dump;
    memset(dump, 0, sizeof(*dump));
    dump->file = fopen(path, "w");
    if (!dump->file) {
        log_error("pac_dump", "Cannot open the PAC-table dump %s: %s", path, strerror(errno));
        return -1;
    }
    dump->buf = malloc(PAC_DUMP_STDIO_BUF_BYTES);
    if (dump->buf) {
        setvbuf(dump->file, dump->buf, _IOFBF, PAC_DUMP_STDIO_BUF_BYTES);
    }
    dump->interval_s = interval_s;
    fprintf(dump->file,
            "# PAC-table snapshot dump (mode %s, %d tables)\n"
            "# interval_sec=%u row_target_per_table_per_snapshot=%d\n"
            "# rows carry pac_value > 0 only; tier: 0=fast 1=slow\n"
            "# rows are thinned by a deterministic key hash at the per-snapshot rate given by "
            "table<i>_subsample\n"
            "t_sec,wl_idx,page_addr,pac_value,access_count,tier\n",
            pact_mode_name(ctx->mode), ctx->nr_domains, interval_s, PAC_DUMP_MAX_ROWS);
    fflush(dump->file);
    dump->next_tsc = rdtsc() + sec_to_tsc(ctx, interval_s);
    log_info("pac_dump", "PAC-table dump: %s (every %u s)", path, interval_s);
    return 0;
}

/* The admission bar of one table in PAC units: PACT admits the top bin
 * (from (bin_count - 1) * bin_width); Tierce admits from its bar. */
static double admission_bar(const pac_domain_t *domain, const char **mode)
{
    const binning_state_t *bin = domain->binning;
    if (bin) {
        *mode = "topbin";
        return bin->bin_count > 0 ? (double)(bin->bin_count - 1) * bin->bin_width : 0.0;
    }
    *mode = "quantile";
    return domain->bar;
}

static void dump_table_header(FILE *fp, int d, const pac_domain_t *domain, uint32_t mask)
{
    const char *mode = "";
    double bar = admission_bar(domain, &mode);
    size_t n = kh_size(domain->table);
    fprintf(fp, "# table%d_admission_bar=%.4f\n", d, bar);
    fprintf(fp, "# table%d_bar_mode=%s table%d_table_entries=%zu table%d_subsample=1/%u", d, mode,
            d, n, d, mask + 1u);
    if (domain->binning) {
        fprintf(fp, " table%d_q1=%.4f table%d_q3=%.4f table%d_bin_width=%.4f table%d_bin_count=%zu",
                d, domain->binning->q1, d, domain->binning->q3, d, domain->binning->bin_width, d,
                domain->binning->bin_count);
    }
    if (domain->pages) {
        fprintf(fp, " table%d_reservoir=%zu", d, domain->pages->count);
    }
    fputc('\n', fp);
}

/* The thinning test runs on the key, so the metadata is read only for
 * written rows. */
static void dump_table_rows(FILE *fp, const pac_domain_t *domain, double t_sec, uint32_t mask)
{
    pac_table_t *table = domain->table;
    for (khint_t x = 0; x < kh_end(table); ++x) {
        if (!kh_exist(table, x) || (mask && (pac_dump_hash(kh_key(table, x)) & mask) != 0)) {
            continue;
        }
        const pac_metadata_t *meta = kh_val(table, x);
        if (!meta || meta->pac_value == 0) {
            continue;
        }
        fprintf(fp, "%.3f,%u,0x%lx,%lu,%u,%u\n", t_sec, (unsigned)meta->workload, meta->page_addr,
                meta->pac_value, meta->access_count, (unsigned)meta->tier);
    }
}

/* Runs on the event-loop thread, which owns every table insert and every
 * bar update. The migration thread changes only the tier, prev_tier and
 * migrating bytes of single entries, so a row's tier can be one migration
 * stale. */
void pac_dump_snapshot(pact_context_t *ctx)
{
    pac_dump_t *dump = &ctx->pac_dump;
    if (!dump->file) {
        return;
    }
    double t_sec = (double)(rdtsc() - ctx->start_tsc) / (double)ctx->tsc_freq_hz;
    fprintf(dump->file, "# snapshot t=%.3f\n", t_sec);
    for (int d = 0; d < ctx->nr_domains; d++) {
        uint32_t mask = pac_dump_subsample_mask(kh_size(ctx->domains[d].table));
        dump_table_header(dump->file, d, &ctx->domains[d], mask);
        dump_table_rows(dump->file, &ctx->domains[d], t_sec, mask);
    }
    fflush(dump->file);
    dump->snapshots++;
}

void pac_dump_close(pact_context_t *ctx)
{
    pac_dump_t *dump = &ctx->pac_dump;
    if (dump->file) {
        fclose(dump->file);
        dump->file = NULL;
        log_info("pac_dump", "PAC-table dump closed (%lu snapshots)", dump->snapshots);
    }
    free(dump->buf);
    dump->buf = NULL;
    dump->next_tsc = 0;
}
