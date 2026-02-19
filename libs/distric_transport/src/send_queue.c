/**
 * @file send_queue.c
 * @brief Per-connection circular send queue — v3 ring buffer.
 *
 * All operations are O(1). No memmove, no compaction.
 *
 * Ring invariants:
 *   tail = (head + len) % capacity
 *   free  = capacity - len
 *   push writes at tail, wrapping if needed (two-segment write)
 *   peek  reads from head up to the first wrap point
 *   consume advances head by n (mod capacity)
 */

#define _DEFAULT_SOURCE

#include "send_queue.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

int send_queue_init(send_queue_t* q, size_t capacity, size_t hwm) {
    if (!q || capacity == 0 || hwm > capacity) return -1;

    q->buf = malloc(capacity);
    if (!q->buf) return -1;

    q->capacity = capacity;
    q->hwm      = hwm;
    q->head     = 0;
    q->len      = 0;
    return 0;
}

void send_queue_destroy(send_queue_t* q) {
    if (!q) return;
    free(q->buf);
    q->buf      = NULL;
    q->capacity = 0;
    q->len      = 0;
    q->head     = 0;
}

/* ============================================================================
 * WRITE PATH — ring push, at most two memcpy
 * ========================================================================= */

int send_queue_push(send_queue_t* q, const void* data, size_t len) {
    if (!q || !data || len == 0) return -1;
    if (len > q->capacity - q->len)  return -1; /* Not enough free space */

    size_t tail    = (q->head + q->len) % q->capacity;
    size_t to_end  = q->capacity - tail;    /* bytes from tail to buffer end */

    if (len <= to_end) {
        /* Single contiguous write — no wrap */
        memcpy(q->buf + tail, data, len);
    } else {
        /* Two-segment write: fill to end, then wrap to start */
        memcpy(q->buf + tail, data,          to_end);
        memcpy(q->buf,        (const uint8_t*)data + to_end, len - to_end);
    }

    q->len += len;
    return 0;
}

/* ============================================================================
 * READ PATH — return contiguous segment from head
 * ========================================================================= */

void send_queue_peek(const send_queue_t* q, const uint8_t** out, size_t* avail) {
    if (!q || !out || !avail) return;

    if (q->len == 0) {
        *out   = NULL;
        *avail = 0;
        return;
    }

    /* How many bytes are contiguous from head before we hit the buffer end? */
    size_t to_end = q->capacity - q->head;
    *out   = q->buf + q->head;
    *avail = (q->len < to_end) ? q->len : to_end;
}

/* ============================================================================
 * CONSUME — O(1) head advance, no data moved
 * ========================================================================= */

void send_queue_consume(send_queue_t* q, size_t n) {
    if (!q || n == 0) return;
    if (n > q->len) n = q->len;

    q->head = (q->head + n) % q->capacity;
    q->len -= n;

    /* When empty, reset head to 0 for cache-line alignment on next push */
    if (q->len == 0) {
        q->head = 0;
    }
}