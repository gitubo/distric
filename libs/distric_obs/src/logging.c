/*
 * logging.c — DistriC Observability Library — Async Structured Logging
 *
 * MPSC ring buffer (multiple producers, single consumer):
 *   Producers: atomic fetch-add to claim slot, publish with store-release.
 *   Consumer:  background thread drains slots in order.
 *
 * Non-blocking guarantee: if ring is full, entry is dropped and
 * DISTRIC_ERR_BUFFER_OVERFLOW is returned immediately.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MAX_LOG_ENTRY_LEN  2048
#define MAX_SPIN_COUNT     4096

typedef enum {
    SLOT_EMPTY  = 0,
    SLOT_FILLED = 1,
} slot_state_t;

typedef struct {
    _Atomic uint32_t state;
    char             data[MAX_LOG_ENTRY_LEN];
    int              data_len;
} log_slot_t;

struct logger_s {
    log_slot_t*      slots;
    uint32_t         mask;          /* buffer_size - 1 */

    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    _Atomic uint32_t refcount;

    int              fd;
    log_mode_t       mode;

    _Atomic bool     shutdown;
    pthread_t        flush_thread;
    bool             flush_started;
};

/* ============================================================================
 * Format helper
 * ========================================================================= */

static const char* level_str(log_level_t level) {
    switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_WARN:  return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_FATAL: return "FATAL";
    default:              return "UNKNOWN";
    }
}

static int format_entry(char* buf, int buf_size,
                         log_level_t level, const char* component,
                         const char* message, va_list ap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    int off = snprintf(buf, (size_t)buf_size,
        "{\"ts\":%lld.%09lld,\"level\":\"%s\",\"component\":\"%s\",\"msg\":\"%s\"",
        (long long)ts.tv_sec, (long long)ts.tv_nsec,
        level_str(level), component ? component : "", message ? message : "");
    if (off < 0 || off >= buf_size) return off;

    /* Variadic key-value pairs */
    const char* key;
    while ((key = va_arg(ap, const char*)) != NULL) {
        const char* val = va_arg(ap, const char*);
        if (!val) break;
        int w = snprintf(buf + off, (size_t)(buf_size - off),
                         ",\"%s\":\"%s\"", key, val);
        if (w > 0) off += w;
        if (off >= buf_size) break;
    }

    if (off < buf_size - 2) {
        buf[off++] = '}';
        buf[off++] = '\n';
        buf[off]   = '\0';
    }
    return off;
}

/* ============================================================================
 * Flush thread
 * ========================================================================= */

static void* flush_thread_fn(void* arg) {
    struct logger_s* lg = (struct logger_s*)arg;

    while (!atomic_load_explicit(&lg->shutdown, memory_order_acquire)) {
        uint64_t tail = atomic_load_explicit(&lg->tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&lg->head, memory_order_acquire);

        if (tail == head) {
            struct timespec ts = { 0, 500000L };  /* 0.5 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        uint32_t idx = (uint32_t)(tail & (uint64_t)lg->mask);
        log_slot_t* slot = &lg->slots[idx];

        uint32_t spin = 0;
        while (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_FILLED) {
            if (++spin > MAX_SPIN_COUNT) break;
            /* busy-wait: producer still writing */
        }
        if (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_FILLED)
            continue;

        if (slot->data_len > 0)
            (void)write(lg->fd, slot->data, (size_t)slot->data_len);

        atomic_store_explicit(&slot->state, SLOT_EMPTY, memory_order_release);
        atomic_store_explicit(&lg->tail, tail + 1, memory_order_release);
    }

    /* Drain remaining entries before shutdown. */
    uint64_t tail = atomic_load_explicit(&lg->tail, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&lg->head, memory_order_acquire);
    while (tail < head) {
        uint32_t idx = (uint32_t)(tail & (uint64_t)lg->mask);
        log_slot_t* slot = &lg->slots[idx];
        uint32_t spin = 0;
        while (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_FILLED
               && ++spin < MAX_SPIN_COUNT) {}
        if (atomic_load_explicit(&slot->state, memory_order_acquire) == SLOT_FILLED) {
            if (slot->data_len > 0)
                (void)write(lg->fd, slot->data, (size_t)slot->data_len);
        }
        atomic_store_explicit(&slot->state, SLOT_EMPTY, memory_order_release);
        atomic_store_explicit(&lg->tail, ++tail, memory_order_release);
        head = atomic_load_explicit(&lg->head, memory_order_acquire);
    }
    return NULL;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================= */

distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode) {
    if (!logger) return DISTRIC_ERR_INVALID_ARG;

    struct logger_s* lg = calloc(1, sizeof(*lg));
    if (!lg) return DISTRIC_ERR_ALLOC_FAILURE;

    uint32_t bsz = RING_BUFFER_SIZE;
    lg->mask = bsz - 1;
    lg->slots = calloc(bsz, sizeof(log_slot_t));
    if (!lg->slots) { free(lg); return DISTRIC_ERR_ALLOC_FAILURE; }
    for (uint32_t i = 0; i < bsz; i++)
        atomic_init(&lg->slots[i].state, SLOT_EMPTY);

    atomic_init(&lg->head,     0);
    atomic_init(&lg->tail,     0);
    atomic_init(&lg->refcount, 1);
    atomic_init(&lg->shutdown, false);
    lg->fd   = fd;
    lg->mode = mode;

    if (mode == LOG_MODE_ASYNC) {
        if (pthread_create(&lg->flush_thread, NULL, flush_thread_fn, lg) != 0) {
            free(lg->slots);
            free(lg);
            return DISTRIC_ERR_THREAD;
        }
        lg->flush_started = true;
    }

    *logger = lg;
    return DISTRIC_OK;
}

void log_retain(logger_t* lg) {
    if (lg) atomic_fetch_add_explicit(&lg->refcount, 1, memory_order_relaxed);
}

void log_release(logger_t* lg) {
    if (!lg) return;
    if (atomic_fetch_sub_explicit(&lg->refcount, 1, memory_order_acq_rel) != 1) return;
    free(lg->slots);
    free(lg);
}

void log_destroy(logger_t* lg) {
    if (!lg) return;
    atomic_store_explicit(&lg->shutdown, true, memory_order_release);
    if (lg->flush_started)
        pthread_join(lg->flush_thread, NULL);
    log_release(lg);
}

/* ============================================================================
 * Write
 * ========================================================================= */

distric_err_t log_write(logger_t* lg, log_level_t level,
                         const char* component, const char* message, ...) {
    if (!lg) return DISTRIC_ERR_INVALID_ARG;

    char entry_buf[MAX_LOG_ENTRY_LEN];
    va_list ap;
    va_start(ap, message);
    int len = format_entry(entry_buf, sizeof(entry_buf), level, component, message, ap);
    va_end(ap);
    if (len <= 0) return DISTRIC_ERR_INVALID_FORMAT;

    if (lg->mode == LOG_MODE_SYNC) {
        (void)write(lg->fd, entry_buf, (size_t)len);
        return DISTRIC_OK;
    }

    /* ASYNC: non-blocking ring buffer write */
    uint64_t head = atomic_load_explicit(&lg->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&lg->tail, memory_order_acquire);
    if (head - tail >= (uint64_t)(lg->mask + 1))
        return DISTRIC_ERR_BUFFER_OVERFLOW;

    uint64_t my_slot = atomic_fetch_add_explicit(&lg->head, 1, memory_order_acq_rel);
    /* Re-check after claim */
    tail = atomic_load_explicit(&lg->tail, memory_order_acquire);
    if (my_slot - tail >= (uint64_t)(lg->mask + 1)) {
        /* Rolled past capacity — drop and roll back. */
        atomic_fetch_sub_explicit(&lg->head, 1, memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    uint32_t idx = (uint32_t)(my_slot & (uint64_t)lg->mask);
    log_slot_t* slot = &lg->slots[idx];

    /* Brief wait for exporter to clear old data in this slot. */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_EMPTY) {
        if (++spin > MAX_SPIN_COUNT) return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    int copy_len = len < MAX_LOG_ENTRY_LEN ? len : MAX_LOG_ENTRY_LEN - 1;
    memcpy(slot->data, entry_buf, (size_t)copy_len);
    slot->data_len = copy_len;
    atomic_store_explicit(&slot->state, SLOT_FILLED, memory_order_release);
    return DISTRIC_OK;
}