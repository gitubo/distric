/**
 * @file tracing.h
 * @brief DistriC Observability — Tracing Internal Implementation Details
 *
 * NOT part of the public API.  Only tracing.c may include this header.
 *
 * =============================================================================
 * ARCHITECTURE — LAYERED DESIGN
 * =============================================================================
 *
 *   Layer 1: Span buffer mechanism (span_buffer_t)
 *     Lock-free MPSC ring buffer for span slots.
 *     Writers: trace_start_span / trace_finish_span (any thread).
 *     Reader:  exporter thread drains filled slots periodically.
 *
 *   Layer 2: Sampling policy state machine (sampling_state_t)
 *     Two-signal adaptive sampling.  Policy config is immutable after init.
 *     in_backpressure is written ONLY by the exporter thread; read by all.
 *
 *     Signal A — queue fill:
 *       Enter backpressure when fill ≥ BP_FILL_ENTER_PCT (75%).
 *       Exit  backpressure when fill < BP_FILL_EXIT_PCT  (50%).
 *
 *     Signal B — sustained drop rate:
 *       Enter when ≥ DROP_RATE_ENTER_THRESHOLD drops in DROP_WINDOW_NS.
 *       Exit after DROP_CLEAR_WINDOW_NS with zero new drops.
 *
 *     Hysteresis (Item #8): separate enter/exit thresholds prevent rapid
 *     toggling under oscillating load.
 *
 *     combined: in_backpressure = Signal_A || Signal_B
 *
 *   Layer 3: Export scheduling (tracer_s)
 *     Exporter thread sleeps for export_interval_ms, wakes, drains buffer,
 *     calls user export_callback.  Refreshes cached_time_ns and
 *     last_export_ns (liveness stamp) on each iteration.
 *
 * =============================================================================
 * MEMORY ORDERING ANNOTATIONS (Item #1)
 * =============================================================================
 *
 * span_buffer_t.head:
 *   - Producers: fetch_add(relaxed) — slot state release-store is the sync.
 *   - Exporter:  load(acquire) — see all produced slots up to head.
 *
 * span_buffer_t.tail:
 *   - Exporter:  store(release) — signals to producers that slots are free.
 *   - Producers: load(acquire) — see latest tail for fullness check.
 *
 * span_slot_t.state:
 *   - Producer fill:  store(FILLED, release) — synchronises span data write.
 *   - Exporter drain: load(acquire) — synchronises span data read.
 *   - Exporter clear: store(EMPTY, release) — makes slot available to producers.
 *
 * sampling_state_t.in_backpressure:
 *   - Exporter writes: store(release) — ensures policy config reads are ordered.
 *   - Producers read:  load(acquire) — see up-to-date backpressure state.
 *     NOTE: relaxed is NOT sufficient here; producers must see the latest
 *     policy decision to correctly route spans (Item #8).
 *
 * cached_time_ns:
 *   - Exporter: store(relaxed) — approximate timestamp; no ordering needed.
 *   - Producers: load(relaxed) — stale by up to export_interval_ms; fine for spans.
 *
 * last_export_ns:
 *   - Exporter: store(relaxed) — approximate liveness stamp.
 *   - Health checker: load(relaxed) — staleness check only.
 *
 * spans_dropped_backpressure, spans_sampled_out, etc.:
 *   - fetch_add(relaxed) — pure monotone event counters.
 *
 * =============================================================================
 * CACHE LINE SEPARATION (Item #3)
 * =============================================================================
 *
 * span_buffer_t.head (producer-hot) and .tail (exporter-hot) are on separate
 * alignas(64) cache lines to eliminate false sharing.
 */

#ifndef DISTRIC_TRACING_INTERNAL_H
#define DISTRIC_TRACING_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdalign.h>

/* ============================================================================
 * Internal defaults and thresholds
 * ========================================================================= */

#define SPAN_BUFFER_DEFAULT_CAPACITY     1024u
#define SPAN_EXPORT_INTERVAL_MS_DEFAULT  5000u

/* Slot state flags — atomic transitions:
 *   EMPTY ─[producer claim+fill]─► FILLED ─[exporter: PROCESSING]─► EMPTY
 */
#define SPAN_SLOT_EMPTY      0u
#define SPAN_SLOT_FILLED     1u
#define SPAN_SLOT_PROCESSING 2u

/* Backpressure thresholds (Item #8 hysteresis) */
#define BP_FILL_ENTER_PCT          75u
#define BP_FILL_EXIT_PCT           50u
#define DROP_WINDOW_NS             1000000000ULL   /* 1 second  */
#define DROP_RATE_ENTER_THRESHOLD  5u
#define DROP_CLEAR_WINDOW_NS       3000000000ULL   /* 3 seconds */

/*
 * Exporter liveness threshold.
 * Tracer considered unhealthy if last_export_ns has not advanced in 3×
 * the export interval (gives headroom for a couple of skipped cycles).
 */
#define TRACER_EXPORTER_STALE_MULTIPLIER 3u

/*
 * Producer slot claim spin bound.  Prevents producers from blocking
 * if the exporter is slow to drain (non-blocking invariant).
 */
#define SPAN_SLOT_CLAIM_MAX_SPIN 64u

/* ============================================================================
 * Layer 1: Span buffer — MPSC ring with cache-line-separated head/tail
 * ========================================================================= */

typedef struct {
    _Atomic uint32_t state;   /* SPAN_SLOT_EMPTY | SPAN_SLOT_FILLED | PROCESSING */
    trace_span_t     span;
} span_slot_t;

typedef struct {
    /* Producer-hot — cache line 0 */
    alignas(64) _Atomic uint64_t head;   /* next slot to claim */
    uint32_t                     capacity;
    uint32_t                     mask;

    /* Consumer-hot — cache line 1 */
    alignas(64) _Atomic uint64_t tail;   /* next slot to drain */

    /* Slots array pointer — separate from hot path fields */
    span_slot_t* slots;
} span_buffer_t;

/* ============================================================================
 * Layer 2: Sampling policy state machine (Item #8)
 *
 * Immutable after init except for in_backpressure / bp_*_signal, which are
 * written ONLY by the exporter thread.
 *
 * Producers read in_backpressure with acquire semantics to ensure they see
 * the most recent policy decision.
 * ========================================================================= */

typedef struct {
    /* Immutable policy configuration */
    uint32_t always_sample;
    uint32_t always_drop;
    uint32_t backpressure_sample;
    uint32_t backpressure_drop;

    /*
     * Active sampling state — written by exporter thread ONLY.
     * Producers read with acquire (see memory ordering section above).
     */
    _Atomic bool     in_backpressure;  /* combined: Signal_A || Signal_B */
    _Atomic bool     bp_fill_signal;   /* Signal A: queue depth          */
    _Atomic bool     bp_drop_signal;   /* Signal B: sustained drop rate  */

    /* Drop-rate window bookkeeping — exporter thread only; no atomics needed */
    uint64_t drop_window_start_ns;
    uint64_t drops_at_window_start;
    uint64_t last_drop_seen_ns;

    /* Rolling sample counter for ratio enforcement (relaxed — counter only) */
    _Atomic uint64_t sample_counter;
} sampling_state_t;

/* ============================================================================
 * Internal Prometheus metric handles
 * ========================================================================= */

typedef struct {
    metric_t* queue_depth;           /* distric_internal_tracer_queue_depth      */
    metric_t* sample_rate_pct;       /* distric_internal_tracer_sample_rate_pct  */
    metric_t* spans_dropped;         /* distric_internal_tracer_drops_total       */
    metric_t* spans_sampled_out;     /* distric_internal_tracer_sampled_out_total */
    metric_t* in_backpressure;       /* distric_internal_tracer_backpressure      */
    metric_t* exporter_alive;        /* distric_internal_tracer_exporter_alive    */
    metric_t* exports_succeeded;     /* distric_internal_tracer_exports_succeeded */
} tracer_internal_metrics_t;

/* ============================================================================
 * Layer 3: Full tracer struct
 * ========================================================================= */

struct tracer_s {
    /* Layer 1: Buffer mechanism */
    span_buffer_t buffer;

    /* Layer 2: Sampling policy */
    sampling_state_t sampling;

    /* Layer 3: Export scheduling */
    uint32_t export_interval_ms;
    void (*export_callback)(trace_span_t*, size_t, void*);
    void*    user_data;

    /*
     * cached_time_ns: CLOCK_MONOTONIC ns refreshed by exporter thread.
     * Producers read via relaxed load to get an approximate timestamp
     * without a clock_gettime() syscall.  May be stale by ≤ export_interval_ms.
     * store(relaxed) / load(relaxed).
     */
    _Atomic uint64_t cached_time_ns;

    /*
     * last_export_ns: monotonic timestamp of last exporter thread iteration
     * (written relaxed by exporter; read relaxed by health checker).
     * Used to detect exporter liveness (Item #5).
     */
    _Atomic uint64_t last_export_ns;

    /* Stats counters (all relaxed — pure event counts) */
    _Atomic uint64_t spans_created;
    _Atomic uint64_t spans_sampled_in;
    _Atomic uint64_t spans_sampled_out;
    _Atomic uint64_t spans_dropped;      /* drops due to backpressure */
    _Atomic uint64_t exports_attempted;
    _Atomic uint64_t exports_succeeded;

    /* Internal Prometheus handles */
    tracer_internal_metrics_t metrics_handles;
    bool                       metrics_registered;

    /* Lifecycle */
    _Atomic uint32_t refcount;
    _Atomic bool     shutdown;
    pthread_t        exporter_thread;
    bool             exporter_started;
};

/* ============================================================================
 * Lifecycle assertions
 * ========================================================================= */

#ifdef NDEBUG
#define TRACER_ASSERT_LIFECYCLE(cond) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define TRACER_ASSERT_LIFECYCLE(cond)                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "[distric_obs] tracer lifecycle assert: %s:%d\n",  \
                    __FILE__, __LINE__);                                         \
            abort();                                                             \
        }                                                                       \
    } while (0)
#endif

/* Debug-only: verify span slot state */
#ifdef NDEBUG
#define TRACER_ASSERT_SLOT_STATE(slot, expected) ((void)0)
#else
#define TRACER_ASSERT_SLOT_STATE(slot, expected)                               \
    do {                                                                        \
        uint32_t _s = atomic_load_explicit(&(slot)->state, memory_order_relaxed); \
        if (_s != (expected)) {                                                 \
            fprintf(stderr,                                                     \
                "[distric_obs] span slot state assert: expected %u got %u %s:%d\n",\
                (unsigned)(expected), (unsigned)_s, __FILE__, __LINE__);        \
            abort();                                                             \
        }                                                                       \
    } while (0)
#endif

#endif /* DISTRIC_TRACING_INTERNAL_H */