/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/*====================================================================*
 * obj-pool.c: O(1) object-pool implementation (see obj-pool.h)
 *
 * Features:
 *   -  Constant-time allocation / free (push-pop on a free-list)
 *   -  Dynamic growth (chunks), amortised O(1) cost
 *   -  Optional backing with Linux hugetlb huge pages
 *====================================================================*/

#define _POSIX_C_SOURCE 200809L /* for mmap, getpagesize, etc. */

#include "obj-pool.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdalign.h> /* alignof()               */
#include <stdint.h>   /* SIZE_MAX                */

/*--------------------------------------------------------------------
 *  Internal data structures
 *-------------------------------------------------------------------*/

typedef struct pool_chunk {
    struct pool_chunk *next; /* linked list of all chunks */
    void *mem;               /* raw memory block           */
    size_t mem_size;         /* size actually obtained     */
    bool is_huge;            /* true if MAP_HUGETLB used  */
} pool_chunk;

/* The opaque type becomes visible only inside this translation unit */

struct object_pool {
    size_t element_size;   /* size requested by the user          */
    size_t object_size;    /* padded size (≥ element_size, ≥ void*) */
    size_t chunk_capacity; /* objects per growth chunk            */
    void *free_list;       /* head of free-list (singly linked)   */
    pool_chunk *chunks;    /* all allocated chunks                */
    size_t total_capacity; /* Σ objects across all chunks          */
    size_t free_count;     /* objects currently on the free-list   */
    bool use_hugepages;    /* try MAP_HUGETLB on Linux            */
};

/*--------------------------------------------------------------------
 *  Helper utilities (static)
 *-------------------------------------------------------------------*/

/* Align `n` up to the next multiple of `align`. `align` must be a power-of-2 */
static inline size_t align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

/* Detect whether MAP_HUGETLB is available (Linux only) */
#if defined(MAP_HUGETLB) && defined(__linux__)
#define HAVE_HUGETLB 1
#else
#define HAVE_HUGETLB 0
#endif

/*
 * alloc_chunk_memory()
 *
 *   Try to allocate `bytes` bytes.  If `want_huge` is true and we are on a
 *   platform that supports MAP_HUGETLB we try a MAP_HUGETLB mmap().
 *   If that fails we fall back to ordinary malloc().
 *
 *   out_is_huge      - receives true if a huge-page mapping succeeded.
 *   out_actual_bytes - receives the true size of the allocation (may be larger
 *                      than the requested size when MAP_HUGETLB is used).
 *
 *   Returns a pointer to the usable memory block (or NULL on error).
 */
static void *alloc_chunk_memory(size_t bytes, bool want_huge, bool *out_is_huge,
                                size_t *out_actual_bytes)
{
    void *mem = NULL;
    bool is_huge = false;
    size_t actual = bytes;

/*--------------------- try huge-pages (if requested) ----------------*/
#if HAVE_HUGETLB
    if (want_huge) {
        const size_t huge_page = 2 * 1024 * 1024; /* 2 MiB default */
        size_t aligned = align_up(bytes, huge_page);
        mem = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                   -1, 0);
        if (mem != MAP_FAILED) {
            is_huge = true;
            actual = aligned;
        } else {
            /* Huge-page allocation failed: fall back to malloc() */
            perror("mmap MAP_HUGETLB failed: falling back to malloc");
            mem = NULL;
        }
    }
#endif /* HAVE_HUGETLB */

    /*--------------------- normal allocation fallback --------------------*/
    if (!mem) {
        mem = malloc(bytes);
        if (!mem) {
            /* malloc already set errno = ENOMEM */
            return NULL;
        }
        is_huge = false;
        actual = bytes;
    }

    if (out_is_huge) {
        *out_is_huge = is_huge;
    }
    if (out_actual_bytes) {
        *out_actual_bytes = actual;
    }
    return mem;
}

/*
 * pool_add_chunk()
 *
 *   Allocate a new chunk containing `count` objects, push all objects onto
 *   the free-list and update bookkeeping.
 *
 *   Returns 0 on success, -1 on error (errno set).
 */
static int pool_add_chunk(object_pool *pool, size_t count)
{
    if (!pool || count == 0) {
        return 0; /* nothing to do */
    }

    /* Guard against size_t overflow */
    if (pool->object_size != 0 && count > SIZE_MAX / pool->object_size) {
        errno = ENOMEM;
        return -1;
    }

    size_t raw_bytes = pool->object_size * count;
    bool is_huge = false;
    size_t actual_bytes = 0;

    void *mem = alloc_chunk_memory(raw_bytes, pool->use_hugepages, &is_huge, &actual_bytes);
    if (!mem) {
        errno = ENOMEM;
        return -1;
    }

    /* Allocate bookkeeping node for the chunk */
    pool_chunk *chunk = malloc(sizeof(*chunk));
    if (!chunk) {
        if (is_huge) {
            munmap(mem, actual_bytes);
        } else {
            free(mem);
        }
        errno = ENOMEM;
        return -1;
    }

    chunk->next = pool->chunks;
    chunk->mem = mem;
    chunk->mem_size = actual_bytes;
    chunk->is_huge = is_huge;
    pool->chunks = chunk;

    pool->total_capacity += count;

    /* Initialise the free-list with the new objects (LIFO order) */
    char *base = (char *)mem;
    for (size_t i = 0; i < count; ++i) {
        void *obj = base + i * pool->object_size;
        *((void **)obj) = pool->free_list; /* store next pointer */
        pool->free_list = obj;
    }
    pool->free_count += count;

    return 0;
}

/*--------------------------------------------------------------------
 *  Public API (implementations)
 *-------------------------------------------------------------------*/

object_pool *pool_create(size_t element_size, size_t initial_capacity, size_t chunk_capacity,
                         bool use_hugepages)
{
    if (element_size == 0) {
        errno = EINVAL;
        return NULL;
    }

    object_pool *p = malloc(sizeof(*p));
    if (!p) {
        /* errno already set by malloc */
        return NULL;
    }

    /* --------------------------------------------------------------
     *   Compute the actual size of each object that lives inside the
     *   pool.  We need enough space for the user-requested data
     *   *and* a pointer (for the free-list) and we must honour the
     *   strictest alignment required by the platform.
     * -------------------------------------------------------------- */
    size_t min_obj = sizeof(void *);         /* space for next pointer   */
    size_t max_align = alignof(max_align_t); /* platform’s max alignment */
    size_t needed = (element_size > min_obj) ? element_size : min_obj;
    size_t obj_sz = align_up(needed, max_align);

    p->element_size = element_size;
    p->object_size = obj_sz;
    p->chunk_capacity = (chunk_capacity ? chunk_capacity : 1024);
    p->free_list = NULL;
    p->chunks = NULL;
    p->total_capacity = 0;
    p->free_count = 0;
    p->use_hugepages = use_hugepages;

    if (initial_capacity) {
        if (pool_add_chunk(p, initial_capacity) != 0) {
            pool_destroy(p);
            return NULL;
        }
    }
    return p;
}

/*---------------------------------------------------------------*/
void pool_destroy(object_pool *pool)
{
    if (!pool) {
        return;
    }

    pool_chunk *c = pool->chunks;
    while (c) {
        pool_chunk *next = c->next;
        if (c->is_huge) {
            munmap(c->mem, c->mem_size);
        } else {
            free(c->mem);
        }
        free(c);
        c = next;
    }
    free(pool);
}

/*---------------------------------------------------------------*/
/* Simple allocation for single-threaded use */
static void *pool_alloc_simple(object_pool *pool)
{
    if (pool->free_list == NULL) {
        /* No free objects: try to grow the pool */
        if (pool_add_chunk(pool, pool->chunk_capacity) != 0) {
            return NULL; /* out of memory */
        }
    }

    /* Take object from free list */
    void *obj = pool->free_list;
    pool->free_list = *((void **)obj);
    pool->free_count--;
    return obj;
}

void *pool_alloc(object_pool *pool)
{
    if (!pool) {
        return NULL;
    }
    return pool_alloc_simple(pool);
}

/*---------------------------------------------------------------*/
void *pool_alloc_zero(object_pool *pool)
{
    void *obj = pool_alloc_simple(pool);
    if (obj) {
        /* Zero only the user-visible portion (the padding bytes need not be cleared) */
        memset(obj, 0, pool->element_size);
    }
    return obj;
}

/*---------------------------------------------------------------*/
void pool_free(object_pool *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }

    /* Simple free - put back on free list */
    *((void **)ptr) = pool->free_list;
    pool->free_list = ptr;
    pool->free_count++;
}

/*---------------------------------------------------------------*/
size_t pool_available(const object_pool *pool)
{
    if (!pool) {
        return 0;
    }

    return pool->free_count;
}

/*---------------------------------------------------------------*/
/*  Diagnostic accessor: defined *after* the struct is complete   */
size_t get_total_capacity(const object_pool *pool)
{
    if (!pool) {
        return 0;
    }

    return pool->total_capacity;
}

size_t get_total_memory(const object_pool *pool)
{
    /* If the caller passes a NULL pointer we simply report zero. */
    if (!pool) {
        return 0;
    }

    size_t total = 0;
    for (const pool_chunk *c = pool->chunks; c != NULL; c = c->next) {
        total += c->mem_size;
    }
    return total;
}

size_t get_element_size(const object_pool *pool)
{
    return pool ? pool->element_size : 0;
}
