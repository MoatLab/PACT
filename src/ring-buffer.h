/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Single-producer / single-consumer (SPSC) lock-free ring buffer.
 *
 * Concurrency contract: exactly ONE thread pushes and exactly ONE (other or
 * same) thread pops. The producer owns `head`; the consumer owns `tail`.
 * Capacity is `size - 1` (one slot is sacrificed to disambiguate full from
 * empty). `size` is rounded up to a power of two so indexing is a mask.
 *
 * Memory ordering (portable, not relying on x86 TSO):
 *   - The producer publishes a slot by storing `head` with release; the
 *     consumer reads `head` with acquire, so the payload write in that slot
 *     happens-before the consumer reads it.
 *   - The consumer frees a slot by storing `tail` with release; the producer
 *     reads `tail` with acquire, so a slot is not overwritten until the
 *     consumer has finished reading it.
 *   - Each side reads its OWN index with relaxed order (it is the sole writer).
 *
 * False-sharing: `head` and `tail` sit on separate 64-byte cache lines, kept
 * apart from each other and from the read-only fields (`buffer`/`size`/`mask`),
 * so the producer's and consumer's hot stores never invalidate each other's
 * line. The instance is cache-line aligned at allocation time so this layout
 * is honored on the heap.
 */

#ifndef RING_BUFFER_CACHELINE
#define RING_BUFFER_CACHELINE 64
#endif

/**
 * @brief Defines a new type-safe SPSC ring buffer implementation.
 *
 * @param type The data type of the elements to be stored in the buffer.
 * @param name A suffix to be added to all function and type names to make them unique.
 */
#define DEFINE_RING_BUFFER(type, name)                                                               \
                                                                                                     \
    typedef struct {                                                                                 \
        /* Read-only after create(): isolated on its own line. */                                    \
        type *buffer;                                                                                \
        size_t size;                                                                                 \
        size_t mask;                                                                                 \
        /* Producer-owned head and consumer-owned tail, each on its own line. */                     \
        alignas(RING_BUFFER_CACHELINE) _Atomic size_t head;                                          \
        alignas(RING_BUFFER_CACHELINE) _Atomic size_t tail;                                          \
        char _pad_end[RING_BUFFER_CACHELINE - sizeof(_Atomic size_t)];                               \
    } ring_buffer_##name##_t;                                                                        \
                                                                                                     \
    static inline ring_buffer_##name##_t *ring_buffer_##name##_create(size_t size)                   \
    {                                                                                                \
        /* Ensure size is a power of 2 for efficient masking */                                      \
        if (size < 2) {                                                                              \
            size = 2;                                                                                \
        }                                                                                            \
        if ((size & (size - 1)) != 0) {                                                              \
            size_t new_size = 1;                                                                     \
            while (new_size < size)                                                                  \
                new_size <<= 1;                                                                      \
            size = new_size;                                                                         \
        }                                                                                            \
                                                                                                     \
        /* aligned_alloc needs size a multiple of alignment; sizeof(struct) is                     \
         * a multiple of RING_BUFFER_CACHELINE because of the alignas above. */ \
        ring_buffer_##name##_t *rb =                                                                 \
            aligned_alloc(RING_BUFFER_CACHELINE, sizeof(ring_buffer_##name##_t));                    \
        if (!rb)                                                                                     \
            return NULL;                                                                             \
        memset(rb, 0, sizeof(*rb));                                                                  \
                                                                                                     \
        rb->buffer = calloc(size, sizeof(type));                                                     \
        if (!rb->buffer) {                                                                           \
            free(rb);                                                                                \
            return NULL;                                                                             \
        }                                                                                            \
                                                                                                     \
        rb->size = size;                                                                             \
        rb->mask = size - 1;                                                                         \
        atomic_store_explicit(&rb->head, 0, memory_order_relaxed);                                   \
        atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);                                   \
                                                                                                     \
        return rb;                                                                                   \
    }                                                                                                \
                                                                                                     \
    static inline void ring_buffer_##name##_destroy(ring_buffer_##name##_t *rb)                      \
    {                                                                                                \
        if (!rb)                                                                                     \
            return;                                                                                  \
        free(rb->buffer);                                                                            \
        free(rb);                                                                                    \
    }                                                                                                \
                                                                                                     \
    /* Producer side. */                                                                             \
    static inline bool ring_buffer_##name##_push(ring_buffer_##name##_t *rb, type value)             \
    {                                                                                                \
        size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);                         \
        size_t next_head = (head + 1) & rb->mask;                                                    \
        if (next_head == atomic_load_explicit(&rb->tail, memory_order_acquire)) {                    \
            return false; /* Buffer is Full */                                                       \
        }                                                                                            \
        rb->buffer[head] = value;                                                                    \
        atomic_store_explicit(&rb->head, next_head, memory_order_release);                           \
        return true;                                                                                 \
    }                                                                                                \
                                                                                                     \
    /* Consumer side. */                                                                             \
    static inline bool ring_buffer_##name##_pop(ring_buffer_##name##_t *rb, type *value)             \
    {                                                                                                \
        size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);                         \
        if (tail == atomic_load_explicit(&rb->head, memory_order_acquire)) {                         \
            return false; /* Buffer is Empty */                                                      \
        }                                                                                            \
        *value = rb->buffer[tail];                                                                   \
        atomic_store_explicit(&rb->tail, (tail + 1) & rb->mask, memory_order_release);               \
        return true;                                                                                 \
    }                                                                                                \
                                                                                                     \
    /* Consumer side: snapshot head once (acquire), drain up to `max`, then                        \
     * publish the new tail once (release). */ \
    static inline int ring_buffer_##name##_pop_batch(ring_buffer_##name##_t *rb, type *values,       \
                                                     int max)                                        \
    {                                                                                                \
        size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);                         \
        size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);                         \
        int count = 0;                                                                               \
        while (count < max && tail != head) {                                                        \
            values[count++] = rb->buffer[tail];                                                      \
            tail = (tail + 1) & rb->mask;                                                            \
        }                                                                                            \
        if (count > 0) {                                                                             \
            atomic_store_explicit(&rb->tail, tail, memory_order_release);                            \
        }                                                                                            \
        return count;                                                                                \
    }                                                                                                \
                                                                                                     \
    /* Approximate occupancy. Safe to call from either side; uses acquire loads                    \
     * so a consumer using it as a drain gate sees published producer writes. */ \
    static inline size_t ring_buffer_##name##_size(ring_buffer_##name##_t *rb)                       \
    {                                                                                                \
        size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);                         \
        size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);                         \
        return (head - tail) & rb->mask;                                                             \
    }

#endif /* RING_BUFFER_H */
