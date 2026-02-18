/**
 * @file logging.h
 * @brief DistriC Observability — Logging Internal Implementation Details
 *
 * NOT part of the public API.  Only logging.c may include this header.
 *
 * =============================================================================
 * THREADING MODEL — ASYNC MODE (MPSC LOCK-FREE RING BUFFER)
 * =============================================================================
 *
 * Producers (any application thread):
 *   1. Format JSON into stack buffer (no heap allocation).
 *   2. Check entry size ≤ max_entry_bytes; if not → drop + oversized_drops++.
 *   3. Check fullness: if (head - tail) >= capacity → drop + total_drops++.
 *   4. Claim slot: idx = atomic_fetch_add(&head, 1, relaxed).
 *   5. Re-verify after claim: idx - tail_snapshot >= capacity → drop.
 *   6. Bounded spin on slot->state == SLOT_EMPTY (≤ SLOT_CLAIM_MAX_SPIN).
 *      If slot still non-empty after limit → drop (prevents blocking).
 *   7. Copy entry data into slot->data.
 *   8. Publish: atomic_store(&slot->state, SLOT_FILLED, release).
 *
 * Consumer (single flush thread, step 8 happens-before step 1 of next cycle):
 *   1. Load tail (thread-local; only written by consumer).
 *   2. Load head with acquire to observe all producer-published slots.
 *   3. Bounded spin on slot->state until SLOT_FILLED (acquire).
 *   4. write() entry to fd (best-effort; errors not fatal).
 *   5. Stamp last_flush_ns (relaxed — approximate liveness signal).
 *   6. Mark slot SLOT_EMPTY with release (makes slot visible to producers).
 *   7. Advance tail with release (makes slot reuse safe for fullness checks).
 *
 * =============================================================================
 * MEMORY ORDERING ANNOTATIONS
 * =============================================================================
 *
 * head:
 *   - Producers: fetch_add(relaxed) — no ordering needed for counter claim.
 *     The slot's release store (step 8) is what synchronises slot contents.
 *   - Consumer: load(acquire) — ensures all producer slot stores are visible
 *     after we observe an increased head count.
 *   - Fullness check by producers: load(relaxed) — approximate; the re-check
 *     after claim catches the precise race.
 *
 * tail:
 *   - Consumer: store(release) — makes the slot reuse visible to producers.
 *   - Producers: load(acquire) — ensures they see the latest released tail.
 *
 * slot->state:
 *   - Producer fill:   store(release) — synchronises slot->data write.
 *   - Consumer drain:  load(acquire)  — synchronises slot->data read.
 *   - Consumer clear:  store(release) — synchronises slot reuse by next prod.
 *   - Producer claim:  load(acquire)  — ensures slot is genuinely empty.
 *
 * total_drops / oversized_drops:
 *   - fetch_add(relaxed) — pure monotone counters; no ordering required.
 *
 * last_flush_ns:
 *   - Consumer: store(relaxed) — approximate timestamp; ordering not required.
 *   - Health checker: load(relaxed) — approx staleness check; no ordering req.
 *
 * shutdown:
 *   - Caller: store(release) — signals intent; all prior state must be visible.
 *   - Consumer: load(acquire) — observes shutdown after all pending stores.
 *
 * =============================================================================
 * CACHE LINE SEPARATION (Item #3)
 * =============================================================================
 *
 * log_ring_buffer_t separates producer-hot (head) and consumer-hot (tail)
 * fields to opposite 64-byte cache lines.  This eliminates false sharing
 * between the flush thread and application producer threads.
 *
 * head → cache line 0: written by all producers → highly contended.
 * tail → cache line 1: written only by flush thread; rarely bounces.
 */

#ifndef DISTRIC_LOGGING_INTERNAL_H
#define DISTRIC_LOGGING_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdalign.h>

/* ============================================================================
 * Internal defaults
 * ========================================================================= */

#define LOG_RING_BUFFER_DEFAULT_CAPACITY  8192u   /* must be power of 2 */
#define LOG_MAX_ENTRY_BYTES_DEFAULT       4096u
#define LOG_CONSUMER_MAX_SPIN             512u    /* spins before yield (consumer) */

/*
 * SLOT_CLAIM_MAX_SPIN: maximum producer spins waiting for a slot to become
 * EMPTY.  If exceeded, the producer drops the entry rather than blocking.
 * This is the primary non-blocking safety valve.  Should be small; the
 * scenario only arises when a producer claims a slot that hasn't been
 * drained yet (very rare under normal load).
 */
#define SLOT_CLAIM_MAX_SPIN 64u

/* Slot state tags — atomic transitions:
 *   EMPTY ─[producer claim+fill]─► FILLED ─[consumer drain]─► EMPTY
 */
#define SLOT_EMPTY  0u
#define SLOT_FILLED 1u

/*
 * Exporter liveness threshold (nanoseconds).
 * Logger is considered unhealthy if last_flush_ns has not advanced in
 * more than this interval.  Default: 30 seconds.
 */
#define LOG_EXPORTER_STALE_NS  (30ULL * 1000000000ULL)

/* ============================================================================
 * Internal types
 * ========================================================================= */

/*
 * log_slot_t: individual ring entry.
 * state is the synchronisation point:
 *   - producer writes data then sets state = FILLED (release)
 *   - consumer reads state (acquire), reads data, sets state = EMPTY (release)
 * data is sized to max_entry_bytes; actual used length is in .len.
 */
typedef struct {
    alignas(64) _Atomic uint32_t state;   /* SLOT_EMPTY | SLOT_FILLED — on own CL */
    size_t                       len;
    char                         data[LOG_MAX_ENTRY_BYTES_DEFAULT];
} log_slot_t;

/*
 * log_ring_buffer_t: MPSC power-of-two ring.
 *
 * Cache-line separation:
 *   head (cache line 0): written by every producer thread → hot.
 *   tail (cache line 1): written only by flush thread → cold for producers.
 *
 * Invariant: (head - tail) uses unsigned wraparound arithmetic for safety.
 *            Both counters are monotonically increasing uint64_t values;
 *            the difference never exceeds capacity under correct operation.
 */
typedef struct {
    /* Producer-hot — cache line 0 */
    alignas(64) _Atomic uint64_t head;   /* next slot to claim (producer incr) */
    size_t                       capacity;
    size_t                       mask;   /* capacity - 1 */
    log_slot_t*                  slots;

    /* Consumer-hot — cache line 1 */
    alignas(64) _Atomic uint64_t tail;   /* next slot to drain (consumer only) */
} log_ring_buffer_t;

/* Internal backpressure metric handles */
typedef struct {
    metric_t* drops_total;         /* gauge: cumulative dropped log entries     */
    metric_t* oversized_drops;     /* gauge: entries dropped for size > max     */
    metric_t* ring_fill_pct;       /* gauge: 0–100 ring fill percentage         */
    metric_t* exporter_alive;      /* gauge: 1 if flush thread healthy, else 0  */
} logger_internal_metrics_t;

struct logger_s {
    int              fd;
    log_mode_t       mode;
    size_t           max_entry_bytes;

    /* Async mode */
    log_ring_buffer_t   ring;
    pthread_t           flush_thread;
    bool                flush_thread_started;

    /*
     * shutdown: store(release) by caller; load(acquire) by consumer.
     * The release ensures all preceding log entries are visible to the
     * consumer before it observes shutdown = true.
     */
    _Atomic bool     shutdown;

    /*
     * last_flush_ns: approximate monotonic timestamp of last successful
     * slot drain by the flush thread.  Written relaxed (liveness signal only;
     * no ordering required).  Read relaxed by health checker.
     * Zero until the flush thread has processed its first entry.
     */
    _Atomic uint64_t last_flush_ns;

    /* Sync mode */
    pthread_mutex_t  sync_lock;

    /* Lifecycle */
    _Atomic uint32_t refcount;

    /*
     * Counters — all incremented with relaxed ordering.
     * Pure monotone event counters; no synchronisation ordering needed.
     */
    _Atomic uint64_t total_drops;       /* ring-full or slot-claim timeout drops */
    _Atomic uint64_t oversized_drops;   /* entries exceeding max_entry_bytes      */

    /* Internal Prometheus handles */
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
#define LOG_ASSERT_LIFECYCLE(cond)                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "[distric_obs] logger lifecycle assert: %s:%d\n",  \
                    __FILE__, __LINE__);                                         \
            abort();                                                             \
        }                                                                       \
    } while (0)
#endif

/* Debug-only: verify impossible slot state transition */
#ifdef NDEBUG
#define LOG_ASSERT_SLOT_STATE(slot, expected_state) ((void)0)
#else
#define LOG_ASSERT_SLOT_STATE(slot, expected_state)                            \
    do {                                                                        \
        uint32_t _s = atomic_load_explicit(&(slot)->state, memory_order_relaxed); \
        if (_s != (expected_state)) {                                           \
            fprintf(stderr,                                                     \
                "[distric_obs] slot state assertion: expected %u got %u %s:%d\n", \
                (unsigned)(expected_state), (unsigned)_s,                       \
                __FILE__, __LINE__);                                             \
            abort();                                                             \
        }                                                                       \
    } while (0)
#endif

#endif /* DISTRIC_LOGGING_INTERNAL_H */