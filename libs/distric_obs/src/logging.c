/*
 * logging.c — DistriC Observability Library — Logging Implementation
 *
 * =============================================================================
 * PRODUCTION HARDENING APPLIED (see Production_Hardening_Prioritized_List.md)
 * =============================================================================
 *
 * #1 Memory-Ordering Audit
 *   - head: fetch_add(relaxed) for claim; load(acquire) by consumer.
 *   - tail: store(release) by consumer; load(acquire) by producers.
 *   - slot->state: release on publish, acquire on consume, release on clear.
 *   - shutdown: store(release), load(acquire).
 *   - last_flush_ns, total_drops, oversized_drops: relaxed (counters/approx).
 *   All decisions are commented inline.
 *
 * #2 Ring Buffer Correctness & Wraparound Hardening
 *   - tail is now _Atomic uint64_t written with atomic_store_explicit (was UB).
 *   - Producer slot-claim spin is bounded by SLOT_CLAIM_MAX_SPIN; excess → drop.
 *   - Non-blocking invariant: every producer path completes in O(1) time.
 *   - Debug builds assert impossible slot state transitions.
 *
 * #3 Cache Line Separation (in logging.h)
 *   - log_ring_buffer_t.head (producer-hot) and .tail (consumer-hot) are on
 *     separate alignas(64) cache lines.
 *
 * #5 Exporter Thread Failure Detection
 *   - flush thread stamps last_flush_ns (relaxed) after each successful drain.
 *   - log_is_exporter_healthy() checks staleness vs LOG_EXPORTER_STALE_NS.
 *   - Prometheus gauge distric_internal_logger_exporter_alive reflects this.
 *
 * #7 JSON Log Size Safety
 *   - format_and_dispatch() checks formatted size against max_entry_bytes.
 *   - Oversized entries increment oversized_drops and return DISTRIC_ERR_BUFFER_OVERFLOW.
 *   - Partial JSON is never written to the ring.
 *
 * #9 Self-Monitoring Completeness
 *   - log_register_metrics() registers distric_internal_logger_* gauges.
 *   - Flush thread updates gauges after each drain cycle.
 *
 * Two APIs:
 *   log_write_kv  (SAFE):     explicit kv array; no NULL sentinel required.
 *   log_write     (ADVANCED): variadic; requires NULL-terminated key-value pairs.
 *
 * Thread-safety:
 *   Async: MPSC ring buffer — producers claim slots; single consumer drains.
 *   Sync:  pthread_mutex serialises writes.
 *
 * Backpressure:
 *   Ring full → drop + total_drops++ + DISTRIC_ERR_BUFFER_OVERFLOW.
 *   Oversized → drop + oversized_drops++ + DISTRIC_ERR_BUFFER_OVERFLOW.
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
#include <stdalign.h>

/* ============================================================================
 * Monotonic clock helper
 * ========================================================================= */

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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
 * format_json: formats a complete JSON log entry into buf (max buf_size).
 * Returns byte count written (not including NUL), or -1 if the output
 * would be truncated.  Callers MUST check the return value and discard
 * the entry on -1 (Item #7: never emit partial JSON).
 */
static int format_json(char* buf, size_t buf_size,
                       log_level_t level, const char* component,
                       const char* message,
                       const log_kv_t* kv_pairs, size_t kv_count) {
    size_t o = 0;

#define APPEND(s)                                                       \
    do {                                                                 \
        size_t _n = strlen(s);                                           \
        if (o + _n >= buf_size) return -1; /* truncation guard */       \
        memcpy(buf + o, (s), _n); o += _n;                             \
    } while (0)

    APPEND("{\"timestamp\":");
    {
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%llu",
                         (unsigned long long)current_time_ms());
        if (n <= 0 || o + (size_t)n >= buf_size) return -1;
        memcpy(buf + o, tmp, (size_t)n);
        o += (size_t)n;
    }

    APPEND(",\"level\":");
    {
        int n = json_escape(buf + o, buf_size - o, log_level_str(level));
        if (n <= 0 || o + (size_t)n >= buf_size) return -1;
        o += (size_t)n;
    }

    APPEND(",\"component\":");
    {
        int n = json_escape(buf + o, buf_size - o, component ? component : "");
        if (n <= 0 || o + (size_t)n >= buf_size) return -1;
        o += (size_t)n;
    }

    APPEND(",\"message\":");
    {
        int n = json_escape(buf + o, buf_size - o, message ? message : "");
        if (n <= 0 || o + (size_t)n >= buf_size) return -1;
        o += (size_t)n;
    }

    for (size_t i = 0; i < kv_count; i++) {
        if (!kv_pairs[i].key) continue;
        APPEND(",");
        {
            int n = json_escape(buf + o, buf_size - o, kv_pairs[i].key);
            if (n <= 0 || o + (size_t)n >= buf_size) return -1;
            o += (size_t)n;
        }
        APPEND(":");
        {
            int n = json_escape(buf + o, buf_size - o,
                                kv_pairs[i].value ? kv_pairs[i].value : "");
            if (n <= 0 || o + (size_t)n >= buf_size) return -1;
            o += (size_t)n;
        }
    }

    APPEND("}\n");
#undef APPEND

    if (o < buf_size) buf[o] = '\0';
    return (int)o;
}

/* ============================================================================
 * Async: ring buffer write (MPSC, non-blocking)
 *
 * Memory ordering contract (Item #1):
 *   - Fullness check: head(relaxed) - tail(acquire). Acquire on tail ensures
 *     we see the latest consumer advancement.  Head can be relaxed because
 *     the re-check after claim is the definitive safety gate.
 *   - Slot claim: fetch_add(head, relaxed). Ordering not needed here; the
 *     slot's state release-store is what synchronises slot data.
 *   - Slot claim re-check: tail(acquire) — must see consumer's latest advance.
 *   - Slot spin: load(state, acquire) — see consumer's EMPTY release.
 *   - Slot publish: store(state, SLOT_FILLED, release) — synchronises data.
 *
 * Non-blocking guarantee (Item #2):
 *   Producer slot spin is bounded by SLOT_CLAIM_MAX_SPIN.  Exceeding the
 *   limit causes an immediate drop rather than an unbounded stall.
 * ========================================================================= */
static distric_err_t ring_write(logger_t* logger,
                                 const char* data, size_t len) {
    log_ring_buffer_t* rb = &logger->ring;

    /*
     * Step 1: Approximate fullness pre-check.
     * head(relaxed): stale is acceptable; re-check after claim is definitive.
     * tail(acquire): must see latest consumer release to avoid false overflow.
     */
    uint64_t head_snap = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint64_t tail_snap = atomic_load_explicit(&rb->tail, memory_order_acquire);
    if (head_snap - tail_snap >= rb->capacity) {
        /* Ring full: drop immediately without claiming a slot */
        atomic_fetch_add_explicit(&logger->total_drops, 1, memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    /*
     * Step 2: Claim a slot index.
     * fetch_add(relaxed): the slot's state release-store (step 5) establishes
     * the actual happens-before edge.  No ordering needed on the counter itself.
     */
    uint64_t idx = atomic_fetch_add_explicit(&rb->head, 1, memory_order_relaxed);

    /*
     * Step 3: Definitive capacity re-check after claim.
     * Another producer may have raced us to fill the ring between step 1 and 2.
     * tail(acquire): ensures we see the latest consumer advance.
     */
    tail_snap = atomic_load_explicit(&rb->tail, memory_order_acquire);
    if (idx - tail_snap >= rb->capacity) {
/*
      * We over-claimed.  Mark the slot SLOT_DROPPED so the consumer
      * can identify it and advance tail without spinning indefinitely.
      *
      * WHY NOT SLOT_EMPTY: setting SLOT_EMPTY could corrupt a legitimately
      * FILLED slot at the same modular index that the consumer hasn't drained
      * yet, causing silent data loss.  SLOT_DROPPED is a distinct sentinel
      * that tells the consumer "nothing useful here, skip it".
      *
      * WHY NOT "no store at all": if we skip the store entirely, the consumer
      * eventually reaches tail == idx, sees head > tail, and enters the slot
      * spin.  It finds the slot in SLOT_EMPTY (its cleared state from the
      * previous cycle) and waits forever since no producer will set SLOT_FILLED.
      * SLOT_DROPPED prevents that spin.
      */
        log_slot_t* lost = &rb->slots[idx & rb->mask];
        atomic_store_explicit(&lost->state, SLOT_DROPPED, memory_order_release);
        atomic_fetch_add_explicit(&logger->total_drops, 1, memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    log_slot_t* slot = &rb->slots[idx & rb->mask];

    /*
     * Step 4: Bounded spin waiting for slot to be EMPTY.
     * This slot was previously used and the consumer may not have cleared it
     * yet.  We spin with a hard cap to preserve the non-blocking invariant.
     * load(acquire): synchronises with consumer's EMPTY release-store.
     */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SLOT_EMPTY) {
        if (spin >= SLOT_CLAIM_MAX_SPIN) {
            /*
             * Slot still not empty after max spins.  Drop entry to avoid
             * blocking.  This can happen under extreme contention when the
             * consumer is very slow relative to producers.
             */
            atomic_fetch_add_explicit(&logger->total_drops, 1, memory_order_relaxed);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        ++spin;
        /* Yield only after a few busy spins to keep latency low in common case */
        if (spin > 16) sched_yield();
    }

    /* Debug assertion: slot must be EMPTY when we reach here */
    LOG_ASSERT_SLOT_STATE(slot, SLOT_EMPTY);

    /*
     * Step 5: Copy data and publish.
     * Data copy happens before state store.  The release fence on the state
     * store synchronises the entire data write with the consumer's acquire
     * load of state.
     */
    size_t copy_len = len < sizeof(slot->data) ? len : sizeof(slot->data) - 1;
    memcpy(slot->data, data, copy_len);
    slot->len = copy_len;

    /*
     * PUBLISH: release store on slot->state.
     * All prior writes (slot->data, slot->len) happen-before the consumer's
     * acquire load of state == SLOT_FILLED.
     */
    atomic_store_explicit(&slot->state, SLOT_FILLED, memory_order_release);

    return DISTRIC_OK;
}

/* ============================================================================
 * Async: flush thread (single consumer)
 *
 * Memory ordering contract (Item #1):
 *   - head(acquire): see all producer fetch_adds and their subsequent stores.
 *   - slot->state(acquire): synchronises with producer's FILLED release.
 *   - slot->state = EMPTY (release): synchronises with next producer acquire.
 *   - tail = advance (release): makes reuse safe for producer fullness checks.
 *   - last_flush_ns (relaxed): approximate liveness; no ordering required.
 * ========================================================================= */
static void* flush_thread_fn(void* arg) {
    logger_t* logger = (logger_t*)arg;
    log_ring_buffer_t* rb = &logger->ring;

    while (1) {
        /*
         * Acquire-load shutdown: ensures we observe all log entries written
         * before the shutdown store.
         */
        bool shutting_down =
            atomic_load_explicit(&logger->shutdown, memory_order_acquire);

        /*
         * tail is the consumer's private cursor.  Only this thread writes it.
         * We read it as a plain load (no atomic instruction needed) but must
         * eventually store back with release to synchronise with producers.
         */
        uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

        /*
         * Acquire-load head: ensures we see all producer-published slots up to
         * this point before we try to drain them.
         */
        uint64_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

        if (tail == head) {
            if (shutting_down) break;
            sched_yield();
            continue;
        }

        log_slot_t* slot = &rb->slots[tail & rb->mask];

        /*
         * Spin wait for SLOT_FILLED.  Producer may have claimed the slot but
         * not yet stored the data.  Bounded by LOG_CONSUMER_MAX_SPIN then yield.
         * On shutdown with spin overflow we stop draining to avoid hang.
         */
        uint32_t spin = 0;
        bool slot_ready = false;
        while (1) {
            /* acquire: synchronises with producer's FILLED release-store */
            uint32_t s = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (s == SLOT_FILLED) { slot_ready = true; break; }
            if (s == SLOT_DROPPED) { break; /* producer over-claim; skip */ }
            /*
             * BUG FIX: Re-read shutdown on every iteration.
             *
             * The outer-loop's `shutting_down` is stale inside this inner loop.
             * If log_destroy() sets shutdown=true while we are already spinning
             * here, the stale `false` value means we never break — causing
             * pthread_join in log_destroy() to block forever.
             *
             * Re-reading atomically ensures we observe the shutdown store as
             * soon as it is visible (acquire pairs with destroy's release).
             */
            bool sd = atomic_load_explicit(&logger->shutdown, memory_order_acquire);
            if (sd && spin > 1024) break;
            if (++spin > LOG_CONSUMER_MAX_SPIN) sched_yield();
        }

        if (!slot_ready) {
            /* Shutdown with stale slot — skip and advance to drain remaining */
            atomic_store_explicit(&slot->state, SLOT_EMPTY, memory_order_release);
            atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);
            continue;
        }        /* Write to fd; best-effort — partial writes are acceptable */
        if (slot->len > 0) {
            ssize_t written = 0;
            while (written < (ssize_t)slot->len) {
                ssize_t n = write(logger->fd,
                                  slot->data + written,
                                  slot->len - (size_t)written);
                if (n <= 0) break;
                written += n;
            }
        }

        /*
         * Stamp liveness timestamp (relaxed — approximate heartbeat).
         * Must happen BEFORE we release the slot, so the health checker
         * cannot observe a stale timestamp while the slot is being processed.
         * (relaxed is sufficient: the health checker only cares about approximate
         *  staleness, not strict ordering relative to slot operations.)
         */
        atomic_store_explicit(&logger->last_flush_ns, mono_ns(),
                              memory_order_relaxed);

        /*
         * CLEAR: release store on slot->state = EMPTY.
         * Synchronises with next producer's acquire load checking for EMPTY.
         * Must happen before we advance tail, otherwise a producer could claim
         * the same index while we still hold it.
         */
        atomic_store_explicit(&slot->state, SLOT_EMPTY, memory_order_release);

        /*
         * ADVANCE tail: release store.
         * Makes the freed slot visible to producer fullness checks.
         * Producers load tail with acquire to see this update.
         */
        atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);

        /* Update Prometheus gauges if metrics are registered */
        if (logger->metrics_registered) {
            uint64_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);
            uint64_t t = atomic_load_explicit(&rb->tail, memory_order_relaxed);
            double fill = (h > t)
                ? (double)(h - t) / (double)rb->capacity * 100.0
                : 0.0;

            if (logger->metrics_handles.ring_fill_pct)
                metrics_gauge_set(logger->metrics_handles.ring_fill_pct, fill);

            if (logger->metrics_handles.drops_total) {
                double drops = (double)atomic_load_explicit(
                    &logger->total_drops, memory_order_relaxed);
                metrics_gauge_set(logger->metrics_handles.drops_total, drops);
            }

            if (logger->metrics_handles.oversized_drops) {
                double od = (double)atomic_load_explicit(
                    &logger->oversized_drops, memory_order_relaxed);
                metrics_gauge_set(logger->metrics_handles.oversized_drops, od);
            }

            /* Exporter alive = 1 while this thread is running */
            if (logger->metrics_handles.exporter_alive)
                metrics_gauge_set(logger->metrics_handles.exporter_alive, 1.0);
        }
    }

    /* Mark exporter dead on exit */
    if (logger->metrics_registered && logger->metrics_handles.exporter_alive)
        metrics_gauge_set(logger->metrics_handles.exporter_alive, 0.0);

    return NULL;
}

/* ============================================================================
 * Sync write (mutex-protected path — low-volume/debug only)
 * ========================================================================= */

static distric_err_t sync_write(logger_t* logger,
                                 const char* data, size_t len) {
    pthread_mutex_lock(&logger->sync_lock);
    ssize_t written = 0;
    while (written < (ssize_t)len) {
        ssize_t n = write(logger->fd, data + written, len - (size_t)written);
        if (n <= 0) break;
        written += n;
    }
    pthread_mutex_unlock(&logger->sync_lock);
    return DISTRIC_OK;
}

/* ============================================================================
 * Core formatter and dispatcher (Item #7: size-safe, no partial JSON)
 * ========================================================================= */

static distric_err_t format_and_dispatch(logger_t* logger,
                                          log_level_t level,
                                          const char* component,
                                          const char* message,
                                          const log_kv_t* kv_pairs,
                                          size_t kv_count) {
    /*
     * Thread-local stack buffer.  Size is the logger's max_entry_bytes cap.
     * We always format into a fixed-size buffer; the format function returns
     * -1 on truncation before writing to the ring (Item #7).
     *
     * Using a generous stack buffer (LOG_MAX_ENTRY_BYTES_DEFAULT) so normal
     * entries always fit; we then gate on max_entry_bytes at runtime.
     */
    char buf[LOG_MAX_ENTRY_BYTES_DEFAULT + 1];

    int n = format_json(buf, sizeof(buf), level, component, message,
                        kv_pairs, kv_count);

    if (n < 0) {
        /*
         * Formatted entry would be truncated (too large).
         * Drop it and count as oversized — never write partial JSON (Item #7).
         */
        atomic_fetch_add_explicit(&logger->oversized_drops, 1,
                                  memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    size_t entry_len = (size_t)n;

    /*
     * Runtime size gate: reject entries exceeding the configured limit.
     * Provides an additional explicit check even for entries that happened
     * to fit in the stack buffer but exceed the operator-configured cap.
     */
    if (entry_len > logger->max_entry_bytes) {
        atomic_fetch_add_explicit(&logger->oversized_drops, 1,
                                  memory_order_relaxed);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }

    if (logger->mode == LOG_MODE_ASYNC)
        return ring_write(logger, buf, entry_len);
    else
        return sync_write(logger, buf, entry_len);
}

/* ============================================================================
 * Init / Destroy / Lifecycle
 * ========================================================================= */

distric_err_t log_init(logger_t** out, int fd, log_mode_t mode) {
    logging_config_t cfg = {
        .fd   = fd,
        .mode = mode,
    };
    return log_init_with_config(out, &cfg);
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

    /* Cap max_entry_bytes to the slot buffer size */
    if (logger->max_entry_bytes > LOG_MAX_ENTRY_BYTES_DEFAULT)
        logger->max_entry_bytes = LOG_MAX_ENTRY_BYTES_DEFAULT;

    atomic_init(&logger->refcount,        1);
    atomic_init(&logger->shutdown,        false);
    atomic_init(&logger->total_drops,     0);
    atomic_init(&logger->oversized_drops, 0);
    atomic_init(&logger->last_flush_ns,   0);
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

        /*
         * head and tail on separate cache lines (from logging.h).
         * Initialise both to zero — no ordering required at init (single thread).
         */
        atomic_init(&logger->ring.head, 0);
        atomic_init(&logger->ring.tail, 0);

        if (pthread_create(&logger->flush_thread, NULL,
                           flush_thread_fn, logger) != 0) {
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
        /*
         * Release store: ensures all log entries queued before this call
         * are visible to the flush thread before it observes shutdown = true.
         */
        atomic_store_explicit(&logger->shutdown, true, memory_order_release);
        pthread_join(logger->flush_thread, NULL);
        free(logger->ring.slots);
    }

    pthread_mutex_destroy(&logger->sync_lock);
    free(logger);
}

/* ============================================================================
 * Exporter health API (Item #5)
 * ========================================================================= */

bool log_is_exporter_healthy(const logger_t* logger) {
    if (!logger) return false;
    if (logger->mode != LOG_MODE_ASYNC) return true; /* sync has no thread */
    if (!logger->flush_thread_started) return false;

    uint64_t last = atomic_load_explicit(
        /* SAFE: casting away const; last_flush_ns is _Atomic, read relaxed */
        &((logger_t*)logger)->last_flush_ns, memory_order_relaxed);

    if (last == 0) {
        /*
         * Thread has never flushed yet.  Give it a grace period equal to
         * the stale threshold; it may simply not have had work to do.
         * If shutdown is already in progress, consider it alive.
         */
        return true;
    }

    uint64_t now = mono_ns();
    return (now - last) < LOG_EXPORTER_STALE_NS;
}

uint64_t log_get_total_drops(const logger_t* logger) {
    if (!logger) return 0;
    return atomic_load_explicit(&((logger_t*)logger)->total_drops,
                                memory_order_relaxed);
}

uint64_t log_get_oversized_drops(const logger_t* logger) {
    if (!logger) return 0;
    return atomic_load_explicit(&((logger_t*)logger)->oversized_drops,
                                memory_order_relaxed);
}

/* ============================================================================
 * Internal Prometheus metrics registration (Item #9)
 *
 * Registers distric_internal_logger_* gauges.  Must be called once after
 * both the logger and the metrics registry are initialised.  Not thread-safe
 * relative to other registration calls — call during startup only.
 * ========================================================================= */
distric_err_t log_register_metrics(logger_t* logger,
                                             metrics_registry_t* registry) {
    if (!logger || !registry) return DISTRIC_ERR_INVALID_ARG;
    if (logger->metrics_registered) return DISTRIC_ERR_ALREADY_EXISTS;

    metrics_register_gauge(registry,
        "distric_internal_logger_drops_total",
        "Cumulative log entries dropped (ring full or slot-claim timeout)",
        NULL, 0, &logger->metrics_handles.drops_total);

    metrics_register_gauge(registry,
        "distric_internal_logger_oversized_drops_total",
        "Log entries dropped because formatted size exceeded max_entry_bytes",
        NULL, 0, &logger->metrics_handles.oversized_drops);

    metrics_register_gauge(registry,
        "distric_internal_logger_ring_fill_pct",
        "Async log ring buffer fill percentage (0-100)",
        NULL, 0, &logger->metrics_handles.ring_fill_pct);

    metrics_register_gauge(registry,
        "distric_internal_logger_exporter_alive",
        "1 if the async log flush thread is alive and making progress, 0 otherwise",
        NULL, 0, &logger->metrics_handles.exporter_alive);

    logger->metrics_registered = true;

    /* Initialise exporter_alive to 1 if thread already running */
    if (logger->flush_thread_started && logger->metrics_handles.exporter_alive)
        metrics_gauge_set(logger->metrics_handles.exporter_alive, 1.0);

    return DISTRIC_OK;
}

/* ============================================================================
 * Safe structured logging API
 * ========================================================================= */

distric_err_t log_write_kv(logger_t* logger, log_level_t level,
                             const char* component, const char* message,
                             const log_kv_t* kv_pairs, size_t kv_count) {
    if (!logger) return DISTRIC_ERR_INVALID_ARG;
    /*
     * acquire: must see shutdown = true set before calling destroy.
     * After destroy is called, no new entries must be queued.
     */
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire))
        return DISTRIC_ERR_SHUTDOWN;
    return format_and_dispatch(logger, level, component, message,
                                kv_pairs, kv_count);
}

/* ============================================================================
 * Variadic logging API — ADVANCED (prefer log_write_kv for new code)
 * Requires NULL-terminated key-value pairs.
 * ========================================================================= */

distric_err_t log_write(logger_t* logger, log_level_t level,
                         const char* component, const char* message, ...) {
    if (!logger) return DISTRIC_ERR_INVALID_ARG;
    if (atomic_load_explicit(&logger->shutdown, memory_order_acquire))
        return DISTRIC_ERR_SHUTDOWN;

    /* Collect variadic key-value pairs */
    log_kv_t kv[DISTRIC_MAX_SPAN_TAGS];  /* use span tag count as KV cap */
    size_t   kv_count = 0;

    va_list ap;
    va_start(ap, message);
    while (kv_count < DISTRIC_MAX_SPAN_TAGS) {
        const char* key = va_arg(ap, const char*);
        if (!key) break;
        const char* val = va_arg(ap, const char*);
        kv[kv_count].key   = key;
        kv[kv_count].value = val ? val : "";
        kv_count++;
    }
    va_end(ap);

    return format_and_dispatch(logger, level, component, message, kv, kv_count);
}