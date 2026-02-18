/**
 * @file logging_internal.h
 * @brief DistriC Observability — Logging Internal Implementation Details
 *
 * NOT part of the public API.  Only logging.c may include this header.
 *
 * Threading model (async mode):
 *   Producers:
 *     1. Format JSON into thread-local buffer (stack; no heap allocation).
 *     2. Check fullness: if head - tail >= capacity → drop + increment
 *        internal drops counter + return DISTRIC_ERR_BUFFER_OVERFLOW.
 *     3. Claim slot: atomic_fetch_add(&rb->head, 1) → unique index.
 *     4. Copy formatted entry into slot.
 *     5. Publish slot: atomic_store(slot->state, SLOT_FILLED, release).
 *
 *   Consumer (single background flush thread):
 *     1. Load tail to find next slot.
 *     2. Spin ≤ MAX_SPIN iterations on slot->state until SLOT_FILLED.
 *     3. write() entry to fd; mark slot SLOT_EMPTY (release).
 *     4. Advance tail.
 *
 * Invariant: tail is only written by the consumer thread.
 *            head is written by producers via atomic fetch-add.
 *            Fullness check: (head - tail) >= capacity → full.
 */

#ifndef DISTRIC_LOGGING_INTERNAL_H
#define DISTRIC_LOGGING_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>

/* ============================================================================
 * Internal defaults
 * ========================================================================= */

#define LOG_RING_BUFFER_DEFAULT_CAPACITY  8192u   /* must be power of 2 */
#define LOG_MAX_ENTRY_BYTES_DEFAULT       4096u
#define LOG_CONSUMER_MAX_SPIN             128u     /* spins before yield */

/* Slot state tags */
#define SLOT_EMPTY  0u
#define SLOT_FILLED 1u

/* ============================================================================
 * Internal types
 * ========================================================================= */

typedef struct {
    _Atomic uint32_t state;       /* SLOT_EMPTY | SLOT_FILLED */
    char             data[LOG_MAX_ENTRY_BYTES_DEFAULT];
    size_t           len;
} log_slot_t;

typedef struct {
    log_slot_t*      slots;           /* heap-allocated ring      */
    size_t           capacity;        /* power-of-2 slot count    */
    size_t           mask;            /* capacity - 1             */
    _Atomic uint64_t head;            /* producer claim index     */
    _Atomic uint64_t tail;            /* consumer drain index     */
} log_ring_buffer_t;

/* Internal backpressure metric handles */
typedef struct {
    metric_t* drops_total;       /* gauge: cumulative dropped entries */
    metric_t* ring_fill_pct;     /* gauge: 0-100 ring fill percentage */
} logger_internal_metrics_t;

struct logger_s {
    int                 fd;
    log_mode_t          mode;
    size_t              max_entry_bytes;

    /* Async mode */
    log_ring_buffer_t   ring;
    pthread_t           flush_thread;
    bool                flush_thread_started;
    _Atomic bool        shutdown;

    /* Sync mode */
    pthread_mutex_t     sync_lock;

    /* Lifecycle */
    _Atomic uint32_t    refcount;

    /* Internal observability */
    _Atomic uint64_t    total_drops;
    logger_internal_metrics_t metrics_handles;
    bool                       metrics_registered;
};

/* ============================================================================
 * Lifecycle assertions
 * ========================================================================= */

#ifdef NDEBUG
#define LOG_ASSERT_LIFECYCLE(cond) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define LOG_ASSERT_LIFECYCLE(cond)                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[distric_obs] LOG LIFECYCLE VIOLATION: %s "     \
                    "(%s:%d)\n", #cond, __FILE__, __LINE__);                  \
            abort();                                                           \
        }                                                                     \
    } while (0)
#endif

#endif /* DISTRIC_LOGGING_INTERNAL_H */