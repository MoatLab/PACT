#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MoatLab, Virginia Tech.
"""Exercise actual PTE/metadata allocation functions with allocator failures."""
from pathlib import Path
import json
import resource
import subprocess
import sys
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
source = Path(sys.argv[1]).read_text()

def extract(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
struct page {void *pginfo;bool marked;} table;
struct mm_struct {bool htmm_enabled;};
typedef struct page *pgtable_t;
static int __userpte_alloc_gfp=123, token;
static void *pginfo_cache=&token;
static bool fail_table, fail_metadata;
static int table_calls, metadata_calls;
static struct page *__pte_alloc_one(struct mm_struct *mm, int gfp)
{
    assert(gfp==__userpte_alloc_gfp);
    table_calls++;
    return fail_table ? NULL : &table;
}
static void *kmem_cache_alloc(void *cache, int gfp)
{
    assert(cache==pginfo_cache && gfp==__userpte_alloc_gfp);
    metadata_calls++;
    return fail_metadata ? NULL : &token;
}
static void SetPageHtmm(struct page *page)
{
    assert(page==&table && page->pginfo==&token);
    page->marked=true;
}
'''
main = r'''
int main(int argc, char **argv)
{
    struct mm_struct mm={atoi(argv[1])};
    fail_table=atoi(argv[2]);fail_metadata=atoi(argv[3]);
    struct page *result=pte_alloc_one(&mm);
    bool metadata_expected=false;
#ifdef CONFIG_HTMM
    metadata_expected=mm.htmm_enabled && !fail_table;
#endif
    assert(table_calls==1);
    assert(result==(fail_table ? NULL : &table));
    assert(metadata_calls==(int)metadata_expected);
    assert(table.marked==(metadata_expected && !fail_metadata));
    assert(table.pginfo==(metadata_expected && !fail_metadata ? &token : NULL));
    return 0;
}
'''
rows = []
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path/'test.c').write_text(header + extract('static void __pte_alloc_pginfo(') + '\n' + extract('pgtable_t pte_alloc_one(') + main)
    for configured in (False, True):
        binary = path/('htmm' if configured else 'no-htmm')
        subprocess.run(['gcc', '-O0', '-fsanitize=undefined', '-fno-sanitize-recover=all', *(['-DCONFIG_HTMM'] if configured else []), str(path/'test.c'), '-o', str(binary)], check=True)
        for enabled in (0, 1):
            for table_failure in (0, 1):
                for metadata_failure in (0, 1):
                    result = subprocess.run([str(binary), str(enabled), str(table_failure), str(metadata_failure)], capture_output=True, text=True)
                    rows.append({'configured': configured, 'enabled': enabled, 'table_failure': table_failure, 'metadata_failure': metadata_failure, 'pass': result.returncode==0, 'stderr': result.stderr})
print(json.dumps(rows, indent=2))
sys.exit(any(not row['pass'] for row in rows))
