/**
 * @file send_queue.h
 * @brief Internal per-connection send queue with high-water mark backpressure.
 *
 * INTERNAL — do not include from outside distric_transport sources.
 *
 * Design:
 *  - A linear byte buffer holds data that could not be sent immediately
 *    (because the socket returned EAGAIN/EWOULDBLOCK).
 *  - On every tcp_send(), the queue is flushed first; new data is appended
 *    only if there is still a backlog.
 *  - If the queue grows above the high-water mark (HWM), the caller receives
 *    DISTRIC_ERR_BACKPRESSURE so it can apply upstream flow control.
 *  - The queue is NOT thread-safe by itself; callers must hold the
 *    connection's send_lock mutex.
 *
 * Memory layout:
 *
 *   [ consumed | pending data | free space ]
 *     ^head      ^head+len     ^capacity
 *
 *  head: byte offset of first unread byte.
 *  len:  number of bytes currently pending.
 *
 * When head > capacity/2, the buffer is compacted (memmove to front).
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
 * SEND QUEUE STRUCTURE
 * ========================================================================= */

typedef struct {
    uint8_t* buf;       /**< Heap-allocated byte buffer.           */
    size_t   capacity;  /**< Total allocated bytes.                */
    size_t   hwm;       /**< High-water mark: backpressure trigger. */
    size_t   head;      /**< Offset of first pending byte.         */
    size_t   len;       /**< Number of bytes pending.              */
} send_queue_t;

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

/**
 * @brief Initialize the send queue.
 * @param q         Queue to initialize.
 * @param capacity  Total buffer capacity in bytes.
 * @param hwm       High-water mark in bytes (must be <= capacity).
 * @return 0 on success, -1 on allocation failure.
 */
int  send_queue_init(send_queue_t* q, size_t capacity, size_t hwm);

/**
 * @brief Destroy the send queue and free its buffer.
 */
void send_queue_destroy(send_queue_t* q);

/* ============================================================================
 * OPERATIONS
 * ========================================================================= */

/**
 * @brief Append data to the queue.
 *
 * @param q     Send queue.
 * @param data  Data to append.
 * @param len   Number of bytes.
 * @return 0 on success, -1 if capacity would be exceeded.
 */
int  send_queue_push(send_queue_t* q, const void* data, size_t len);

/**
 * @brief Return a pointer and length for the next pending chunk.
 *
 * @param q      Send queue.
 * @param out    [out] Pointer to pending data.
 * @param avail  [out] Number of bytes available.
 */
void send_queue_peek(const send_queue_t* q, const uint8_t** out, size_t* avail);

/**
 * @brief Consume (discard) bytes from the front of the queue.
 *
 * Called after a successful send() to advance the read pointer.
 *
 * @param q    Send queue.
 * @param n    Number of bytes to consume.
 */
void send_queue_consume(send_queue_t* q, size_t n);

/**
 * @brief Compact the buffer if head > capacity/2.
 *
 * Moves pending data to the front to make room for new pushes.
 * Call when a push fails to check if compaction helps.
 */
void send_queue_compact(send_queue_t* q);

/* ============================================================================
 * QUERY
 * ========================================================================= */

/** @return true if there are no pending bytes. */
static inline bool send_queue_empty(const send_queue_t* q) {
    return q->len == 0;
}

/** @return true if pending bytes exceed the high-water mark. */
static inline bool send_queue_above_hwm(const send_queue_t* q) {
    return q->len >= q->hwm;
}

/** @return Number of pending bytes. */
static inline size_t send_queue_pending(const send_queue_t* q) {
    return q->len;
}

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_SEND_QUEUE_H */