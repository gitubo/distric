/**
 * @file send_queue.c
 * @brief Per-connection send queue implementation.
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
 * OPERATIONS
 * ========================================================================= */

void send_queue_compact(send_queue_t* q) {
    if (!q || q->head == 0) return;
    if (q->len > 0) {
        memmove(q->buf, q->buf + q->head, q->len);
    }
    q->head = 0;
}

int send_queue_push(send_queue_t* q, const void* data, size_t len) {
    if (!q || !data || len == 0) return -1;

    /* Free space from current head+len to capacity */
    size_t free_after = q->capacity - (q->head + q->len);

    if (free_after < len) {
        /* Try compacting first */
        send_queue_compact(q);
        free_after = q->capacity - q->len;
    }

    if (free_after < len) return -1;  /* Still not enough space */

    memcpy(q->buf + q->head + q->len, data, len);
    q->len += len;
    return 0;
}

void send_queue_peek(const send_queue_t* q, const uint8_t** out, size_t* avail) {
    if (!q || !out || !avail) return;
    *out   = q->buf + q->head;
    *avail = q->len;
}

void send_queue_consume(send_queue_t* q, size_t n) {
    if (!q || n == 0) return;
    if (n > q->len) n = q->len;
    q->head += n;
    q->len  -= n;
    /* Compact eagerly when head exceeds half capacity */
    if (q->len > 0 && q->head > q->capacity / 2) {
        send_queue_compact(q);
    } else if (q->len == 0) {
        q->head = 0;  /* Reset cheaply when empty */
    }
}