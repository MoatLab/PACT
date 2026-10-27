/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/*====================================================================*
 * obj-pool.h: O(1) object-pool with optional hugetlb backing
 *
 * The pool's internal layout is hidden. If you need diagnostic data
 * (total number of objects ever allocated) use the accessor
 * `get_total_capacity()`.
 *====================================================================*/

#ifndef PACT_OBJ_POOL_H
#define PACT_OBJ_POOL_H

#include <stddef.h>  /* size_t */
#include <stdbool.h> /* bool */

#ifdef __cplusplus
extern "C" {
#endif

/* ----- opaque handle ------------------------------------------------- */
typedef struct object_pool object_pool;

/* ----- creation / destruction ---------------------------------------- */
object_pool *pool_create(size_t element_size, size_t initial_capacity, size_t chunk_capacity,
                         bool use_hugepages);

void pool_destroy(object_pool *pool);

/* ----- allocation / de-allocation ------------------------------------ */
void *pool_alloc(object_pool *pool);
void *pool_alloc_zero(object_pool *pool);
void pool_free(object_pool *pool, void *ptr);

/* ----- diagnostics --------------------------------------------------- */
/* Number of objects that are currently free (debug / statistics).   */
size_t pool_available(const object_pool *pool);

/* Total number of objects that have been allocated across all
 * chunks ever created for the pool.  This is a read-only value.        */
size_t get_total_capacity(const object_pool *pool);
size_t get_total_memory(const object_pool *pool);
size_t get_element_size(const object_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* __OBJPOOL_H__ */
