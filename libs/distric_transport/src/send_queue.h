/**
 * @file send_queue.h
 * @brief Per-connection circular send queue with HWM-based backpressure.
 *
 * INTERNAL — do not include from outside distric_transport sources.
 *
 * Design (v3 — true ring buffer):
 *  - Fixed-capacity circular byte buffer. No memmove, no compaction.
 *  - head:  read index (modulo capacity).
 *  - len:   bytes currently buffered.
 *  - tail:  derived as (head + len) % capacity.
 *
 * Push behaviour:
 *  - Data may wrap around the end of the buffer (two-segment write).
 *  - Returns -1 if capacity would be exceeded (caller must backpressure).
 *
 * Peek behaviour:
 *  - Returns a pointer and length for the CONTIGUOUS segment at head.
 *  - If data wraps, this is LESS than len.  The caller must loop:
 *      peek → send → consume  until send_queue_empty().
 *
 * Thread safety:
 *  - NOT thread-safe.  The owner must hold the connection's send_lock.
 *
 * Why this is better than the previous linear-buffer model:
 *  - No memmove on consume: O(1) head advance.
 *  - No memmove on push: O(1) wraparound write (at most two memcpy calls).
 *  - Cache-friendly sequential reads until the wrap point.
 *  - Under backpressure the buffer stays full and spins O(1) per token.
 */

#ifndef DISTRIC_SEND_QUEUE_H
#define DISTRIC_SEND_QUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DEFAULT CONSTANTS
 * ========================================================================= */

#define SEND_QUEUE_DEFAULT_CAPACITY  (64u  * 1024u)   /* 64 KB  */
#define SEND_QUEUE_DEFAULT_HWM       (48u  * 1024u)   /* 48 KB (75%) */

/* ============================================================================
 * STRUCTURE
 * ========================================================================= */

typedef struct {
    uint8_t* buf;       /**< Heap-allocated ring buffer.           */
    size_t   capacity;  /**< Total allocated bytes (power of 2 preferred). */
    size_t   hwm;       /**< High-water mark: backpressure trigger. */
    size_t   head;      /**< Index of first pending byte.          */
    size_t   len;       /**< Number of bytes currently buffered.   */
} send_queue_t;

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

/**
 * @brief Initialise the ring-buffer send queue.
 * @param q         Queue to initialise.
 * @param capacity  Total ring capacity in bytes (> 0).
 * @param hwm       High-water mark (must be <= capacity).
 * @return 0 on success, -1 on bad args or allocation failure.
 */
int  send_queue_init(send_queue_t* q, size_t capacity, size_t hwm);

/**
 * @brief Destroy the queue and free its buffer.
 */
void send_queue_destroy(send_queue_t* q);

/* ============================================================================
 * WRITE PATH
 * ========================================================================= */

/**
 * @brief Append @p len bytes from @p data into the ring buffer.
 *
 * Uses at most two memcpy calls (split at wrap-around point).
 * Returns -1 without modifying the buffer if there is not enough free space.
 *
 * @param q     Send queue.
 * @param data  Source buffer.
 * @param len   Number of bytes to append.
 * @return 0 on success, -1 if insufficient capacity.
 */
int  send_queue_push(send_queue_t* q, const void* data, size_t len);

/* ============================================================================
 * READ PATH
 * ========================================================================= */

/**
 * @brief Return a pointer and length for the next CONTIGUOUS pending chunk.
 *
 * Due to ring-buffer wrap-around, the returned length may be less than
 * send_queue_pending().  The caller must loop: peek → send → consume
 * until send_queue_empty().
 *
 * @param q      Send queue.
 * @param out    [out] Pointer to the start of pending data.
 * @param avail  [out] Number of contiguous bytes available.
 */
void send_queue_peek(const send_queue_t* q, const uint8_t** out, size_t* avail);

/**
 * @brief Advance the read pointer by @p n bytes (O(1), no data moved).
 *
 * @param q  Send queue.
 * @param n  Number of bytes to mark as consumed.
 */
void send_queue_consume(send_queue_t* q, size_t n);

/* ============================================================================
 * QUERY (inline)
 * ========================================================================= */

/** @return true if there are no buffered bytes. */
static inline bool send_queue_empty(const send_queue_t* q) {
    return q->len == 0;
}

/** @return true if buffered bytes >= high-water mark. */
static inline bool send_queue_above_hwm(const send_queue_t* q) {
    return q->len >= q->hwm;
}

/** @return Number of buffered bytes. */
static inline size_t send_queue_pending(const send_queue_t* q) {
    return q->len;
}

/** @return Number of free bytes remaining. */
static inline size_t send_queue_free(const send_queue_t* q) {
    return q->capacity - q->len;
}

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_SEND_QUEUE_H */