/**
 * @file tracing_internal.h
 * @brief DistriC Observability — Tracing Internal Implementation Details
 *
 * NOT part of the public API.  Only tracing.c may include this header.
 *
 * Architecture — layered design (Improvement #7):
 *
 *   Layer 1: Span buffer mechanism (span_buffer_t)
 *     Lock-free MPSC ring buffer for span slots.
 *     Writers: trace_start_span / trace_finish_span (any thread).
 *     Reader:  exporter thread drains filled slots.
 *
 *   Layer 2: Sampling policy state machine (sampling_state_t)
 *     Two-signal adaptive sampling.  Policy state is data-driven and immutable
 *     after initialization except for the in_backpressure flag, which is only
 *     written by the exporter thread (no concurrency on writes).
 *
 *     Signal A (queue fill):
 *       Enter backpressure when fill ≥ BP_FILL_ENTER_PCT (75%).
 *       Exit  backpressure when fill < BP_FILL_EXIT_PCT  (50%).
 *
 *     Signal B (sustained drop rate):
 *       Enter when ≥ DROP_RATE_ENTER_THRESHOLD drops occur in DROP_WINDOW_NS.
 *       Exit after DROP_CLEAR_WINDOW_NS of zero new drops.
 *
 *     in_backpressure = Signal_A || Signal_B
 *
 *   Layer 3: Export scheduling (tracer_s)
 *     Exporter thread sleeps for export_interval_ms, wakes, drains buffer,
 *     calls user export_callback.  Also refreshes cached_time_ns so producers
 *     can read an approximate timestamp via relaxed atomic load (no syscall).
 *
 * Thread-safety:
 *   - trace_start_span / trace_finish_span: lock-free; any thread.
 *   - sampling_state_update: exporter thread only; no concurrent writes.
 *   - trace_get_stats: relaxed atomic reads; any thread; approximate.
 *   - trace_register_metrics: one-time call; serialize with init.
 */

#ifndef DISTRIC_TRACING_INTERNAL_H
#define DISTRIC_TRACING_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>

/* ============================================================================
 * Internal defaults and thresholds
 * ========================================================================= */

#define SPAN_BUFFER_DEFAULT_CAPACITY   1024u   /* must be power of 2 */
#define SPAN_EXPORT_INTERVAL_MS_DEFAULT 5000u

/* Slot state flags */
#define SPAN_SLOT_EMPTY      0u
#define SPAN_SLOT_FILLED     1u
#define SPAN_SLOT_PROCESSING 2u

/* Backpressure thresholds */
#define BP_FILL_ENTER_PCT          75u
#define BP_FILL_EXIT_PCT           50u
#define DROP_WINDOW_NS             1000000000ULL   /* 1 second  */
#define DROP_RATE_ENTER_THRESHOLD  5u
#define DROP_CLEAR_WINDOW_NS       3000000000ULL   /* 3 seconds */

/* ============================================================================
 * Layer 1: Span buffer mechanism
 * ========================================================================= */

typedef struct {
    _Atomic uint32_t state;
    trace_span_t     span;
} span_slot_t;

typedef struct {
    span_slot_t*     slots;
    uint32_t         capacity;
    uint32_t         mask;         /* capacity - 1 */
    _Atomic uint64_t head;         /* producer claim index */
    _Atomic uint64_t tail;         /* exporter drain index */
} span_buffer_t;

/* ============================================================================
 * Layer 2: Sampling policy state machine
 *
 * All policy config is immutable after tracer_init (sampling field is copied
 * by value and never modified).
 * in_backpressure is an atomic flag written ONLY by the exporter thread.
 * ========================================================================= */

typedef struct {
    /* Immutable policy configuration (set at init, never changed) */
    uint32_t always_sample;
    uint32_t always_drop;
    uint32_t backpressure_sample;
    uint32_t backpressure_drop;

    /* Active sampling state (written by exporter thread only) */
    _Atomic bool     in_backpressure;  /* combined signal A || B */
    _Atomic bool     bp_fill_signal;   /* signal A */
    _Atomic bool     bp_drop_signal;   /* signal B */

    /* Drop-rate window bookkeeping (exporter thread only; no atomics needed) */
    uint64_t drop_window_start_ns;
    uint64_t drops_at_window_start;
    uint64_t last_drop_seen_ns;

    /* Rolling sample counter for ratio enforcement */
    _Atomic uint64_t sample_counter;
} sampling_state_t;

/* ============================================================================
 * Internal backpressure metric handles
 * ========================================================================= */

typedef struct {
    metric_t* queue_depth;
    metric_t* sample_rate_pct;
    metric_t* spans_dropped;
    metric_t* spans_sampled_out;
    metric_t* in_backpressure;
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
    uint32_t         export_interval_ms;
    void (*export_callback)(trace_span_t*, size_t, void*);
    void*            user_data;

    /*
     * cached_time_ns: CLOCK_MONOTONIC ns refreshed by exporter thread.
     * Producers read via relaxed load to get an approximate timestamp
     * without paying the cost of a clock_gettime() syscall.
     */
    _Atomic uint64_t cached_time_ns;

    /* Counters (all written lock-free from producers or exporter) */
    _Atomic uint64_t spans_created;
    _Atomic uint64_t spans_sampled_in;
    _Atomic uint64_t spans_sampled_out;
    _Atomic uint64_t spans_dropped_backpressure;
    _Atomic uint64_t exports_attempted;
    _Atomic uint64_t exports_succeeded;

    /* Lifecycle */
    _Atomic uint32_t refcount;
    _Atomic bool     shutdown;
    pthread_t        exporter_thread;
    bool             exporter_started;

    /* Optional Prometheus integration */
    tracer_internal_metrics_t metrics_handles;
    bool                      metrics_registered;
};

/* ============================================================================
 * Lifecycle assertions
 * ========================================================================= */

#ifdef NDEBUG
#define TRACER_ASSERT_LIFECYCLE(cond) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define TRACER_ASSERT_LIFECYCLE(cond)                                       \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[distric_obs] TRACER LIFECYCLE VIOLATION: %s " \
                    "(%s:%d)\n", #cond, __FILE__, __LINE__);                  \
            abort();                                                           \
        }                                                                     \
    } while (0)
#endif

#endif /* DISTRIC_TRACING_INTERNAL_H */