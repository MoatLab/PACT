/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* cgroup.c: the per-workload cgroup files the runtime reads. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgroup.h"

int pact_cgroup_path(const char *word, char *out, size_t len)
{
    int n = (word[0] == '/') ? snprintf(out, len, "%s", word)
                             : snprintf(out, len, "%s/%s", PACT_CGROUP_ROOT, word);
    if (n < 0 || (size_t)n >= len) {
        return -1;
    }
    return 0;
}

int pact_cgroup_node0_cap(const char *path, uint64_t *bytes)
{
    char file[512], text[64];
    if (snprintf(file, sizeof(file), "%s/memory.max_at_node0", path) >= (int)sizeof(file)) {
        return -1;
    }
    FILE *fp = fopen(file, "r");
    if (!fp) {
        return -1;
    }
    bool ok = fgets(text, sizeof(text), fp) != NULL;
    fclose(fp);
    if (!ok) {
        return -1;
    }
    text[strcspn(text, "\n")] = '\0';
    if (text[0] < '0' || text[0] > '9') {
        return -2; /* "max" (no cap) or not a number */
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') {
        return -2;
    }
    /* The patched kernel stores "no limit" for a write of "max" or of a byte
     * count at or above its page-counter maximum (LONG_MAX / page size
     * pages), and the file then reads "max". A value at or above that
     * maximum in bytes is no finite cap either, so it is rejected. */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        page = 4096;
    }
    uint64_t unlimited = (uint64_t)(LONG_MAX / page) * (uint64_t)page;
    if ((uint64_t)value >= unlimited) {
        return -2;
    }
    *bytes = (uint64_t)value;
    return 0;
}

int pact_cgroup_switch(const char *path, const char *name)
{
    char file[512], text[64];
    if (snprintf(file, sizeof(file), "%s/%s", path, name) >= (int)sizeof(file)) {
        return -1;
    }
    FILE *fp = fopen(file, "r");
    if (!fp) {
        return -1;
    }
    bool ok = fgets(text, sizeof(text), fp) != NULL;
    fclose(fp);
    if (!ok) {
        return -1;
    }
    text[strcspn(text, "\n")] = '\0';
    if (strcmp(text, "[enabled] disabled") == 0) {
        return 1;
    }
    if (strcmp(text, "enabled [disabled]") == 0) {
        return 0;
    }
    return -1;
}

int pact_cgroup_stat_sum(const char *path, const char *const *keys, int nr_keys, uint64_t *sum)
{
    char file[512], key[128];
    if (snprintf(file, sizeof(file), "%s/memory.stat", path) >= (int)sizeof(file)) {
        return -1;
    }
    FILE *fp = fopen(file, "r");
    if (!fp) {
        return -1;
    }
    unsigned long long value;
    uint64_t total = 0;
    bool found = false;
    while (fscanf(fp, "%127s %llu", key, &value) == 2) {
        for (int i = 0; i < nr_keys; i++) {
            if (strcmp(key, keys[i]) == 0) {
                total += value;
                found = true;
                break;
            }
        }
    }
    fclose(fp);
    if (!found) {
        return -1;
    }
    *sum = total;
    return 0;
}
