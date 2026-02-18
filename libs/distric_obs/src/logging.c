/*
 * logging.c — DistriC Observability Library — Logging Implementation
 *
 * Two APIs:
 *   log_write_kv  (SAFE):     explicit kv array; no NULL sentinel required.
 *   log_write     (ADVANCED): variadic; requires NULL-terminated key-value pairs.
 *
 * Both route to the same internal format_and_write() function.
 *
 * Thread-safety:
 *   Async: MPSC ring buffer — multiple producers claim slots via atomic
 *          fetch-add; single consumer drains.  Producers never block.
 *   Sync:  pthread_mutex serialises writes.
 *
 * Backpressure:
 *   When the ring buffer is ≥ capacity, the producer increments
 *   logger->total_drops (atomic) and returns DISTRIC_ERR_BUFFER_OVERFLOW.
 *   If metrics are registered, the background thread updates the gauge.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include "distric_obs/logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <assert.h>

/* ============================================================================
 * JSON formatting helpers
 * ========================================================================= */

static const char* log_level_str(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_FATAL: return "FATAL";
        default:              return "UNKNOWN";
    }
}

static int json_escape(char* dst, size_t dsz, const char* src) {
    if (!src) { return snprintf(dst, dsz, "\"\""); }
    size_t o = 0;
    if (o < dsz) dst[o++] = '"';
    for (const char* p = src; *p && o + 4 < dsz; p++) {
        switch (*p) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:   dst[o++] = *p;                     break;
        }
    }
    if (o < dsz) dst[o++] = '"';
    if (o < dsz) dst[o] = '\0';
    return (int)o;
}

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * Core formatter: writes JSON into buf (max buf_size bytes).
 * kv_pairs may be NULL if kv_count == 0.
 * Returns number of bytes written (not including NUL), or -1 on truncation.
 */
static int format_entry(char* buf, size_t buf_size,
                         log_level_t level, const char* component,
                         const char* message,
                         const log_kv_t* kv_pairs, size_t kv_count) {
    size_t o = 0;
    char scratch[512];

    auto inline void append_str(const char* s) {
        size_t len = strlen(s);
        if (o + len < buf_size) { memcpy(buf + o, s, len); o += len; }
    }
    auto inline void append_json_str(const char* s) {
        int n = json_escape(buf + o, buf_size - o, s);
        if (n > 0) o += (size_t)n;
    }

    /* Avoid GCC nested function extension for portability; use a flag approach */
    (void)scratch;

    /* Build JSON manually using snprintf */
    int w;

#define APPEND(fmt, ...) \
    do { w = snprintf(buf + o, buf_size - o, fmt, ##__VA_ARGS__); \
         if (w > 0) o += (size_t)w; } while (0)

    APPEND("{\"timestamp\":%llu,\"level\":\"%s\",\"component\":",
           (unsigned long long)current_time_ms(),
           log_level_str(level));

    /* component */
    {
        int n = json_escape(buf + o, buf_size - o, component ? component : "");
        if (n > 0) o += (size_t)n;
    }

    APPEND(",\"message\":");
    {
        int n = json_escape(buf + o, buf_size - o, message ? message : "");
        if (n > 0) o += (size_t)n;
    }

    for (size_t i = 0; i < kv_count; i++) {
        if (!kv_pairs[i].key) continue;
        APPEND(",");
        {
            int n = json_escape(buf + o, buf_size - o, kv_pairs[i].key);
            if (n > 0) o += (size_t)n;
        }
        APPEND(":");
        {
            int n = json_escape(buf + o, buf_size - o,
                                kv_pairs[i].value ? kv_pairs[i].value : "");
            if (n > 0) o += (size_t)n;
        }
    }

    APPEND("}\n");
#undef APPEND

    if (o < buf_size) buf[o] = '\0';
    return (int)o;
}

/* ============================================================================
 * Async: ring buffer operations
 * ========================================================================= */

static distric_err_t ring_write(logger_t* logger,
                                 const char* data, size_t len) {
    log_ring_buffer_t* rb = &logger->ring;

    /* Non-blocking fullness check */
    uint64_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    if (head - tail >= rb->capacity) {
        atomic_fetch_add_explicit(&logger->total_drops, 1, memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    /* Claim a slot */
    uint64_t idx = atomic_fetch_add_explicit(&rb->head, 1, memory_order_relaxed);

    /* Re-check: another producer may have raced us past capacity */
    if (idx - atomic_load_explicit(&rb->tail, memory_order_acquire) >= rb->capacity) {
        /* We claimed a slot we can't use; just drop */
        atomic_fetch_add_explicit(&logger->total_drops, 1, memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    log_slot_t* slot = &rb->slots[idx & rb->mask];

    /* Wait for slot to be EMPTY (consumer may still be processing it) */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_EMPTY) {
        if (++spin > LOG_CONSUMER_MAX_SPIN) sched_yield();
    }

    size_t copy_len = len < sizeof(slot->data) ? len : sizeof(slot->data) - 1;
    memcpy(slot->data, data, copy_len);
    slot->len = copy_len;
    atomic_store_explicit(&slot->state, SLOT_FILLED, memory_order_release);

    return DISTRIC_OK;
}

static void* flush_thread_fn(void* arg) {
    logger_t* logger = (logger_t*)arg;
    log_ring_buffer_t* rb = &logger->ring;

    while (1) {
        bool shutting_down =
            atomic_load_explicit(&logger->shutdown, memory_order_acquire);
        uint64_t tail = rb->tail;  /* Only written by this thread */
        uint64_t head =
            atomic_load_explicit(&rb->head, memory_order_acquire);

        if (tail == head) {
            if (shutting_down) break;
            sched_yield();
            continue;
        }

        log_slot_t* slot = &rb->slots[tail & rb->mask];

        /* Spin wait for slot to be filled */
        uint32_t spin = 0;
        while (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_FILLED) {
            if (shutting_down && spin > 1000) goto done;
            if (++spin > LOG_CONSUMER_MAX_SPIN) sched_yield();
        }

        /* Write to fd; ignore partial writes in this best-effort system */
        ssize_t written = 0;
        while (written < (ssize_t)slot->len) {
            ssize_t n = write(logger->fd, slot->data + written,
                              slot->len - written);
            if (n <= 0) break;
            written += n;
        }

        atomic_store_explicit(&slot->state, SLOT_EMPTY, memory_order_release);
        rb->tail = tail + 1;  /* Single writer; no atomic needed */

        /* Update backpressure metrics if registered */
        if (logger->metrics_registered && logger->metrics_handles.ring_fill_pct) {
            uint64_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);
            uint64_t t = rb->tail;
            double fill = (h > t) ? ((double)(h - t) / (double)rb->capacity * 100.0)
                                   : 0.0;
            metrics_gauge_set(logger->metrics_handles.ring_fill_pct, fill);
        }
        if (logger->metrics_registered && logger->metrics_handles.drops_total) {
            uint64_t drops =
                atomic_load_explicit(&logger->total_drops, memory_order_relaxed);
            metrics_gauge_set(logger->metrics_handles.drops_total, (double)drops);
        }
    }
done:
    return NULL;
}

/* ============================================================================
 * Sync write
 * ========================================================================= */

static distric_err_t sync_write(logger_t* logger, const char* data, size_t len) {
    pthread_mutex_lock(&logger->sync_lock);
    ssize_t written = 0;
    while (written < (ssize_t)len) {
        ssize_t n = write(logger->fd, data + written, len - written);
        if (n <= 0) break;
        written += n;
    }
    pthread_mutex_unlock(&logger->sync_lock);
    return DISTRIC_OK;
}

/* ============================================================================
 * Core format-and-dispatch (shared by both public APIs)
 * ========================================================================= */

static distric_err_t format_and_dispatch(logger_t* logger,
                                          log_level_t level,
                                          const char* component,
                                          const char* message,
                                          const log_kv_t* kv_pairs,
                                          size_t kv_count) {
    /* Thread-local stack buffer — no heap allocation on hot path */
    char buf[LOG_MAX_ENTRY_BYTES_DEFAULT];
    int len = format_entry(buf, sizeof(buf),
                            level, component, message,
                            kv_pairs, kv_count);
    if (len <= 0) return DISTRIC_ERR_INVALID_ARG;

    if (logger->mode == LOG_MODE_ASYNC)
        return ring_write(logger, buf, (size_t)len);
    else
        return sync_write(logger, buf, (size_t)len);
}

/* ============================================================================
 * Public API
 * ========================================================================= */

distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode) {
    logging_config_t cfg = { .fd = fd, .mode = mode };
    return log_init_with_config(logger, &cfg);
}

distric_err_t log_init_with_config(logger_t** out, const logging_config_t* config) {
    if (!out || !config) return DISTRIC_ERR_INVALID_ARG;
    if (config->fd < 0)  return DISTRIC_ERR_INVALID_ARG;

    logger_t* logger = calloc(1, sizeof(*logger));
    if (!logger) return DISTRIC_ERR_ALLOC_FAILURE;

    logger->fd   = config->fd;
    logger->mode = config->mode;
    logger->max_entry_bytes = config->max_entry_bytes
                              ? config->max_entry_bytes
                              : LOG_MAX_ENTRY_BYTES_DEFAULT;

    atomic_init(&logger->refcount, 1);
    atomic_init(&logger->shutdown, false);
    atomic_init(&logger->total_drops, 0);
    logger->metrics_registered = false;

    if (pthread_mutex_init(&logger->sync_lock, NULL) != 0) {
        free(logger);
        return DISTRIC_ERR_INIT_FAILED;
    }

    if (config->mode == LOG_MODE_ASYNC) {
        /* Determine ring capacity (power of 2; bounded by hard cap) */
        size_t cap = config->ring_buffer_capacity
                     ? config->ring_buffer_capacity
                     : LOG_RING_BUFFER_DEFAULT_CAPACITY;
        /* Round up to power of 2 */
        size_t p2 = 1;
        while (p2 < cap) p2 <<= 1;
        if (p2 > DISTRIC_MAX_RING_BUFFER) p2 = DISTRIC_MAX_RING_BUFFER;
        cap = p2;

        logger->ring.slots = calloc(cap, sizeof(log_slot_t));
        if (!logger->ring.slots) {
            pthread_mutex_destroy(&logger->sync_lock);
            free(logger);
            return DISTRIC_ERR_ALLOC_FAILURE;
        }

        for (size_t i = 0; i < cap; i++)
            atomic_init(&logger->ring.slots[i].state, SLOT_EMPTY);

        logger->ring.capacity = cap;
        logger->ring.mask     = cap - 1;
        atomic_init(&logger->ring.head, 0);
        logger->ring.tail = 0;

        if (pthread_create(&logger->flush_thread, NULL, flush_thread_fn, logger) != 0) {
            free(logger->ring.slots);
            pthread_mutex_destroy(&logger->sync_lock);
            free(logger);
            return DISTRIC_ERR_INIT_FAILED;
        }
        logger->flush_thread_started = true;
    }

    *out = logger;
    return DISTRIC_OK;
}

void log_retain(logger_t* logger) {
    if (!logger) return;
    LOG_ASSERT_LIFECYCLE(
        atomic_load_explicit(&logger->refcount, memory_order_relaxed) > 0);
    atomic_fetch_add_explicit(&logger->refcount, 1, memory_order_relaxed);
}

void log_release(logger_t* logger) {
    if (!logger) return;
    uint32_t prev = atomic_fetch_sub_explicit(&logger->refcount, 1,
                                               memory_order_acq_rel);
    LOG_ASSERT_LIFECYCLE(prev > 0);
    if (prev == 1) log_destroy(logger);
}

void log_destroy(logger_t* logger) {
    if (!logger) return;

    if (logger->mode == LOG_MODE_ASYNC && logger->flush_thread_started) {
        atomic_store_explicit(&logger->shutdown, true, memory_order_release);
        pthread_join(logger->flush_thread, NULL);
        free(logger->ring.slots);
    }

    pthread_mutex_destroy(&logger->sync_lock);
    free(logger);
}

/* ============================================================================
 * Safe structured logging API (Improvement #6)
 * ========================================================================= */

distric_err_t log_write_kv(logger_t* logger, log_level_t level,
                             const char* component, const char* message,
                             const log_kv_t* kv_pairs, size_t kv_count) {
    if (!logger) return DISTRIC_ERR_INVALID_ARG;
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire))
        return DISTRIC_ERR_SHUTDOWN;
    return format_and_dispatch(logger, level, component, message,
                                kv_pairs, kv_count);
}

/* ============================================================================
 * Variadic logging API — ADVANCED / UNSAFE (Improvement #6)
 * Prefer log_write_kv() for new production code.
 * ========================================================================= */

distric_err_t log_write(logger_t* logger, log_level_t level,
                         const char* component, const char* message, ...) {
    if (!logger) return DISTRIC_ERR_INVALID_ARG;
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire))
        return DISTRIC_ERR_SHUTDOWN;

    /* Collect variadic key-value pairs into a stack array */
    log_kv_t kv_stack[DISTRIC_MAX_SPAN_TAGS]; /* reuse tag count as max kv */
    size_t   kv_count = 0;

    va_list ap;
    va_start(ap, message);
    while (kv_count < DISTRIC_MAX_SPAN_TAGS) {
        const char* key = va_arg(ap, const char*);
        if (!key) break;
        const char* val = va_arg(ap, const char*);
        kv_stack[kv_count].key   = key;
        kv_stack[kv_count].value = val;
        kv_count++;
    }
    va_end(ap);

    return format_and_dispatch(logger, level, component, message,
                                kv_stack, kv_count);
}

/* ============================================================================
 * Backpressure metrics registration (Improvement #4)
 * ========================================================================= */

distric_err_t log_register_metrics(logger_t* logger, metrics_registry_t* registry) {
    if (!logger || !registry) return DISTRIC_ERR_INVALID_ARG;
    if (logger->metrics_registered)  return DISTRIC_ERR_ALREADY_EXISTS;

    distric_err_t err;

    err = metrics_register_gauge(registry,
        "distric_internal_log_drops_total",
        "Cumulative number of log entries dropped due to ring buffer overflow",
        NULL, 0, &logger->metrics_handles.drops_total);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_log_ring_fill_pct",
        "Async log ring buffer fill percentage (0-100)",
        NULL, 0, &logger->metrics_handles.ring_fill_pct);
    if (err != DISTRIC_OK) return err;

    logger->metrics_registered = true;
    return DISTRIC_OK;
}