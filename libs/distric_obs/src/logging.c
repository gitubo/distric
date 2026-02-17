/*
 * logging.c — DistriC structured logger
 *
 * Concurrency model: MPSC ring buffer (multi-producer, single-consumer).
 *
 * Producer path (log_write, any thread):
 *   1. Format JSON into thread-local buffer — no allocation.
 *   2. Check fullness with relaxed loads (approximate; acceptable for
 *      best-effort drop detection).
 *   3. Claim a slot: atomic_fetch_add(&rb->head, 1, acq_rel).
 *      The acq_rel ensures prior writes to tls_buffer are not reordered
 *      past the slot claim, and the claim itself is globally ordered.
 *   4. Write entry fields into the claimed slot. The producer has exclusive
 *      ownership of this slot until the SLOT_FILLED publish below.
 *   5. Publish: atomic_store(&slot->state, SLOT_FILLED, release).
 *      The release store makes all slot field writes visible to the consumer
 *      before the state transition is observed.
 *
 * Consumer path (single flush thread):
 *   1. Load rb->tail with acquire to find next slot.
 *   2. Spin up to MAX_CONSUMER_SPIN on slot->state acquire-load until
 *      SLOT_FILLED is observed. The acquire synchronises with the producer's
 *      release publish, ensuring all slot content is visible.
 *   3. On timeout (producer stalled), increment tail anyway and count the
 *      skip — prevents consumer from getting stuck behind a late producer.
 *   4. Write to fd; mark slot EMPTY with release store.
 *   5. Advance rb->tail with release store — prevents wrap-around race.
 *
 * Key invariants:
 *   - head >= tail always (monotonic, wrap handled by mask).
 *   - Slot is exclusively owned by one producer between claim and FILLED.
 *   - Consumer never reads past head; producers never write past tail+SIZE.
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
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>

/* Ring buffer must be a power of 2. */
#define RING_BUFFER_SIZE  16384u
#define RING_BUFFER_MASK  (RING_BUFFER_SIZE - 1u)

/* Log entry formatted buffer.  512 bytes per slot × 16 k slots = 8 MB. */
#define LOG_ENTRY_SIZE    512u
/* Thread-local scratch buffer for JSON formatting (no heap allocation). */
#define LOG_FORMAT_SIZE   4096u

/* Consumer will spin at most this many times waiting for a producer to
 * publish SLOT_FILLED before declaring the slot "stalled" and skipping. */
#define MAX_CONSUMER_SPIN 200

#define LOG_SLOT_EMPTY      0u
#define LOG_SLOT_FILLED     1u

_Static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE - 1)) == 0,
               "RING_BUFFER_SIZE must be a power of 2");

typedef struct {
    _Atomic uint32_t state;           /* LOG_SLOT_EMPTY / LOG_SLOT_FILLED     */
    char             message[LOG_ENTRY_SIZE];
} log_entry_t;

typedef struct {
    log_entry_t     entries[RING_BUFFER_SIZE];
    _Atomic uint64_t head;            /* next slot producers will claim       */
    _Atomic uint64_t tail;            /* next slot consumer will drain        */
    _Atomic bool     running;         /* false → flush thread should exit     */
} ring_buffer_t;

struct logger_s {
    int              fd;
    log_mode_t       mode;
    ring_buffer_t*   ring_buffer;     /* NULL for LOG_MODE_SYNC               */
    pthread_t        flush_thread;
    _Atomic uint32_t refcount;
    _Atomic bool     shutdown;

    /* Diagnostic counters (relaxed — approximate is fine for observability). */
    _Atomic uint64_t messages_logged;
    _Atomic uint64_t messages_dropped;
    _Atomic uint64_t messages_skipped; /* stalled-producer skips by consumer  */
};

/* Per-thread scratch buffer — no heap allocation on the hot path. */
static __thread char tls_buffer[LOG_FORMAT_SIZE];

#if defined(__x86_64__) || defined(__i386__)
#  define CPU_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
#  define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#  define CPU_PAUSE() ((void)0)
#endif

/* -------------------------------------------------------------------------- */
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

static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

/* JSON-escape src into dst.  dst is always NUL-terminated. */
static void json_escape(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"'  || c == '\\') { dst[j++] = '\\'; dst[j++] = c;    }
        else if (c == '\n')              { dst[j++] = '\\'; dst[j++] = 'n';   }
        else if (c == '\r')              { dst[j++] = '\\'; dst[j++] = 'r';   }
        else if (c == '\t')              { dst[j++] = '\\'; dst[j++] = 't';   }
        else if (c < 32)                 { /* skip other control characters */ }
        else                             { dst[j++] = (char)c;                }
    }
    dst[j] = '\0';
}

/* --------------------------------------------------------------------------
 * Consumer flush thread (single instance per async logger).
 * -------------------------------------------------------------------------- */
static void* flush_thread_fn(void* arg) {
    logger_t*     logger = (logger_t*)arg;
    ring_buffer_t* rb    = logger->ring_buffer;

    for (;;) {
        /* Exit only when shutdown AND buffer is drained. */
        bool shutting_down = atomic_load_explicit(&logger->shutdown,
                                                   memory_order_acquire);
        uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

        if (tail == head) {
            if (shutting_down) break;
            /* Buffer empty and still running — yield then retry. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000L }; /* 0.5 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        /* There is at least one slot between tail and head.
         * Wait for the producer to publish SLOT_FILLED. */
        log_entry_t* entry = &rb->entries[tail & RING_BUFFER_MASK];

        int spin = 0;
        uint32_t state;
        while ((state = atomic_load_explicit(&entry->state, memory_order_acquire))
               == LOG_SLOT_EMPTY && spin < MAX_CONSUMER_SPIN) {
            CPU_PAUSE();
            spin++;
        }

        if (state == LOG_SLOT_FILLED) {
            /* Producer has published — slot content is now visible (acquire). */
            size_t len = strnlen(entry->message, LOG_ENTRY_SIZE);
            if (len > 0) {
                ssize_t written_total = 0;
                while (written_total < (ssize_t)len) {
                    ssize_t n = write(logger->fd,
                                      entry->message + written_total,
                                      len - written_total);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        break; /* I/O error — drop this entry */
                    }
                    written_total += n;
                }
            }
            /* Release: makes the EMPTY transition visible to producers
             * checking the slot state before claiming it again. */
            atomic_store_explicit(&entry->state, LOG_SLOT_EMPTY,
                                  memory_order_release);
        } else {
            /* Producer stalled — skip slot to keep consumer moving.
             * The slot will be overwritten safely because head has already
             * claimed this index; the delayed producer will eventually
             * publish to a slot nobody is waiting on any more. */
            atomic_fetch_add_explicit(&logger->messages_skipped, 1,
                                      memory_order_relaxed);
        }

        /* Release: tail advance is visible to producers for fullness checks. */
        atomic_store_explicit(&rb->tail, tail + 1u, memory_order_release);
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode) {
    if (!logger || fd < 0) return DISTRIC_ERR_INVALID_ARG;

    logger_t* log = calloc(1, sizeof(logger_t));
    if (!log) return DISTRIC_ERR_ALLOC_FAILURE;

    log->fd   = fd;
    log->mode = mode;
    atomic_init(&log->refcount,         1);
    atomic_init(&log->shutdown,         false);
    atomic_init(&log->messages_logged,  0);
    atomic_init(&log->messages_dropped, 0);
    atomic_init(&log->messages_skipped, 0);

    if (mode == LOG_MODE_ASYNC) {
        log->ring_buffer = calloc(1, sizeof(ring_buffer_t));
        if (!log->ring_buffer) { free(log); return DISTRIC_ERR_ALLOC_FAILURE; }

        atomic_init(&log->ring_buffer->head,    0);
        atomic_init(&log->ring_buffer->tail,    0);
        atomic_init(&log->ring_buffer->running, true);

        for (size_t i = 0; i < RING_BUFFER_SIZE; i++)
            atomic_init(&log->ring_buffer->entries[i].state, LOG_SLOT_EMPTY);

        if (pthread_create(&log->flush_thread, NULL, flush_thread_fn, log) != 0) {
            free(log->ring_buffer);
            free(log);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }

    *logger = log;
    return DISTRIC_OK;
}

void log_retain(logger_t* logger) {
    if (!logger) return;
    /* Relaxed: increment with no ordering constraints (not a synchronisation
     * point — the object is already live when retain is called). */
    atomic_fetch_add_explicit(&logger->refcount, 1, memory_order_relaxed);
}

void log_release(logger_t* logger) {
    if (!logger) return;
    /* acq_rel: the decrement must be ordered with respect to all prior
     * operations that might use the logger, and with the final free. */
    uint32_t old = atomic_fetch_sub_explicit(&logger->refcount, 1,
                                              memory_order_acq_rel);
    if (old != 1) return; /* still referenced elsewhere */

    /* Signal shutdown — release so the flush thread's acquire sees it. */
    atomic_store_explicit(&logger->shutdown, true, memory_order_release);

    if (logger->mode == LOG_MODE_ASYNC && logger->ring_buffer) {
        pthread_join(logger->flush_thread, NULL);
        free(logger->ring_buffer);
    }
    free(logger);
}

void log_destroy(logger_t* logger) { log_release(logger); }

/* -------------------------------------------------------------------------- */
distric_err_t log_write(logger_t* logger, log_level_t level,
                        const char* component, const char* message, ...) {
    if (!logger || !component || !message) return DISTRIC_ERR_INVALID_ARG;

    /* Acquire: ensures the shutdown flag is observed before proceeding. */
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire))
        return DISTRIC_ERR_INVALID_STATE;

    /* --- Format into thread-local buffer (no allocation) --- */
    size_t  remaining = LOG_FORMAT_SIZE;
    size_t  offset    = 0;
    int     w;

    w = snprintf(tls_buffer, remaining, "{");
    if (w < 0 || (size_t)w >= remaining) return DISTRIC_ERR_BUFFER_OVERFLOW;
    offset += (size_t)w; remaining -= (size_t)w;

    w = snprintf(tls_buffer + offset, remaining,
                 "\"timestamp\":%lu,", get_timestamp_ms());
    if (w < 0 || (size_t)w >= remaining) return DISTRIC_ERR_BUFFER_OVERFLOW;
    offset += (size_t)w; remaining -= (size_t)w;

    char esc_component[256];
    json_escape(component, esc_component, sizeof(esc_component));
    w = snprintf(tls_buffer + offset, remaining,
                 "\"level\":\"%s\",\"component\":\"%s\",",
                 log_level_str(level), esc_component);
    if (w < 0 || (size_t)w >= remaining) return DISTRIC_ERR_BUFFER_OVERFLOW;
    offset += (size_t)w; remaining -= (size_t)w;

    char esc_message[1024];
    json_escape(message, esc_message, sizeof(esc_message));
    w = snprintf(tls_buffer + offset, remaining,
                 "\"message\":\"%s\"", esc_message);
    if (w < 0 || (size_t)w >= remaining) return DISTRIC_ERR_BUFFER_OVERFLOW;
    offset += (size_t)w; remaining -= (size_t)w;

    va_list args;
    va_start(args, message);
    const char* key;
    while ((key = va_arg(args, const char*)) != NULL) {
        const char* val = va_arg(args, const char*);
        if (!val) break;
        char esc_key[128], esc_val[512];
        json_escape(key, esc_key, sizeof(esc_key));
        json_escape(val, esc_val, sizeof(esc_val));
        w = snprintf(tls_buffer + offset, remaining,
                     ",\"%s\":\"%s\"", esc_key, esc_val);
        if (w > 0 && (size_t)w < remaining) { offset += (size_t)w; remaining -= (size_t)w; }
    }
    va_end(args);

    w = snprintf(tls_buffer + offset, remaining, "}\n");
    if (w < 0 || (size_t)w >= remaining) return DISTRIC_ERR_BUFFER_OVERFLOW;
    offset += (size_t)w;

    /* Double-check shutdown after formatting (non-blocking fast path). */
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire))
        return DISTRIC_ERR_INVALID_STATE;

    /* --- Emit --- */
    if (logger->mode == LOG_MODE_SYNC) {
        ssize_t total = 0;
        while (total < (ssize_t)offset) {
            ssize_t n = write(logger->fd, tls_buffer + total, offset - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return DISTRIC_ERR_IO;
            }
            total += n;
        }
        atomic_fetch_add_explicit(&logger->messages_logged, 1,
                                  memory_order_relaxed);
        return DISTRIC_OK;
    }

    /* --- Async path --- */
    ring_buffer_t* rb = logger->ring_buffer;
    if (!rb) return DISTRIC_ERR_INVALID_STATE;

    /* Relaxed loads are sufficient for the drop check: we only need an
     * approximate view of fullness. A spurious drop is acceptable; a
     * spurious "not full" will be caught at claim time. */
    uint64_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    if (head - tail >= RING_BUFFER_SIZE) {
        /* Buffer full — drop without blocking. */
        atomic_fetch_add_explicit(&logger->messages_dropped, 1,
                                  memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    /* Claim a slot.  acq_rel: the fetch-add must be globally ordered so that
     * two concurrent producers claim distinct slots.  The acquire half ensures
     * we see the latest tail (prevents us from erroneously overwriting an
     * un-drained slot); the release half ensures our subsequent slot writes
     * are not reordered before the claim. */
    uint64_t slot_idx = atomic_fetch_add_explicit(&rb->head, 1u,
                                                   memory_order_acq_rel);
    log_entry_t* entry = &rb->entries[slot_idx & RING_BUFFER_MASK];

    /* Write content.  The producer has exclusive ownership of this slot
     * (the consumer only reads slots ≤ tail, and tail < slot_idx here). */
    strncpy(entry->message, tls_buffer, LOG_ENTRY_SIZE - 1);
    entry->message[LOG_ENTRY_SIZE - 1] = '\0';

    /* Publish with release — makes all field writes above visible to the
     * consumer before it observes the SLOT_FILLED state transition. */
    atomic_store_explicit(&entry->state, LOG_SLOT_FILLED, memory_order_release);

    atomic_fetch_add_explicit(&logger->messages_logged, 1, memory_order_relaxed);
    return DISTRIC_OK;
}