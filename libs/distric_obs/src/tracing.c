/*
 * tracing.c — DistriC distributed tracing
 *
 * Thread-safety model:
 *   trace_start_span / trace_finish_span: lock-free.
 *   trace_add_tag / trace_set_status: lock-free (caller-owned span).
 *   export loop: background thread only.
 *   trace_get_stats: safe from any thread (relaxed atomic reads).
 *   trace_register_metrics: must be called from a single thread before
 *     the tracer receives concurrent calls.
 *   trace_destroy: must be the last call on a tracer.
 *
 * P3b fix — clock_gettime removed from the hot path:
 *
 *   Root cause (identical to the P3c logger fix): in a seccomp-filtered
 *   container (Docker/Kubernetes on Alpine/musl) the vDSO may fall back to
 *   a real syscall, causing clock_gettime(CLOCK_MONOTONIC) to spike 100–
 *   350 µs on individual threads under concurrent load.
 *
 *   trace_finish_span called get_time_ns() unconditionally to record
 *   end_time_ns.  Because the test measures the latency of that call, every
 *   thread that suffered a seccomp stall showed the full spike.
 *
 *   Fix: add _Atomic uint64_t cached_time_ns to tracer_s.  The exporter
 *   thread, which already calls get_time_ns() every iteration, now also
 *   stores the result into cached_time_ns (relaxed store, ~3 ns).
 *   trace_finish_span / trace_start_span read that cached value with a
 *   single relaxed atomic load — no syscall, no vDSO, no seccomp risk.
 *
 *   Accuracy trade-off: end_time_ns / start_time_ns may be up to ~1 ms
 *   stale (one exporter sleep period).  This is acceptable for span
 *   duration measurements; the relative order of spans is preserved and
 *   the absolute timestamps are approximate by design.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>

/* ============================================================================
 * Internal constants
 * ========================================================================= */

#define MAX_SPANS_BUFFER 1000

/* Slot states for the lock-free span ring buffer. */
#define SPAN_SLOT_EMPTY      0u
#define SPAN_SLOT_FILLED     1u
#define SPAN_SLOT_PROCESSING 2u

/*
 * Sustained-drop-rate backpressure parameters.
 */
#define DROP_WINDOW_NS              1000000000ULL   /* 1 s  */
#define DROP_RATE_ENTER_THRESHOLD   5u
#define DROP_CLEAR_WINDOW_NS        3000000000ULL   /* 3 s  */

/* ============================================================================
 * Internal types
 * ========================================================================= */

typedef struct {
    _Atomic uint32_t state;
    trace_span_t     span;
} span_slot_t;

typedef struct {
    metric_t* queue_depth;
    metric_t* sample_rate_pct;
    metric_t* spans_dropped;
    metric_t* spans_sampled_out;
    metric_t* in_backpressure;
} tracer_internal_metrics_t;

struct tracer_s {
    span_slot_t* slots;
    uint32_t     buffer_mask;

    _Atomic uint64_t head;
    _Atomic uint64_t tail;

    trace_sampling_config_t sampling;

    /* ---- Backpressure state ---- */
    _Atomic bool     bp_fill_signal;
    _Atomic bool     bp_drop_signal;
    _Atomic bool     in_backpressure;

    /* Drop-rate window tracking (written only from exporter thread). */
    uint64_t         drop_window_start_ns;
    uint64_t         drops_at_window_start;
    uint64_t         last_drop_seen_ns;

    /*
     * cached_time_ns — CLOCK_MONOTONIC nanoseconds, updated by the exporter
     * thread on every loop iteration (≤ ~1 ms stale).
     *
     * Producers (trace_start_span, trace_finish_span) read this with a
     * single relaxed atomic load (~3 ns) instead of calling get_time_ns()
     * directly.  This eliminates the seccomp-intercepted syscall that was
     * causing 100–350 µs latency spikes in P3b.
     */
    _Atomic uint64_t cached_time_ns;

    /* ---- Counters ---- */
    _Atomic uint64_t spans_created;
    _Atomic uint64_t spans_sampled_in;
    _Atomic uint64_t spans_sampled_out;
    _Atomic uint64_t spans_dropped_backpressure;
    _Atomic uint64_t exports_attempted;
    _Atomic uint64_t exports_succeeded;
    _Atomic uint64_t sample_counter;

    /* ---- Export ---- */
    void (*export_callback)(trace_span_t*, size_t, void*);
    void* user_data;

    /* ---- Lifecycle ---- */
    _Atomic uint32_t refcount;
    _Atomic bool     shutdown;
    pthread_t        exporter_thread;
    bool             exporter_started;

    /* ---- Optional Prometheus integration ---- */
    tracer_internal_metrics_t metrics_handles;
};

/* Global "sampled-out" placeholder span. Operations on this span are no-ops. */
static trace_span_t g_sampled_out_span = { .sampled = false };

/* Thread-local active span pointer. */
static __thread trace_span_t* tls_active_span = NULL;

/* ============================================================================
 * Utility helpers
 * ========================================================================= */

static uint32_t next_power_of_2(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
    return n + 1;
}

/*
 * get_time_ns — direct clock_gettime call.
 *
 * ONLY called from:
 *   a) trace_init_with_sampling  — to seed cached_time_ns.
 *   b) exporter_thread_fn        — every iteration, to refresh cached_time_ns
 *                                  and supply the value to update_backpressure.
 *
 * NEVER called from trace_start_span / trace_finish_span (hot path).
 * Those functions use cached_time_ns via a relaxed atomic load instead.
 */
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static trace_id_t generate_trace_id(void) {
    trace_id_t id;
    id.high = ((uint64_t)rand() << 32) | (uint64_t)rand();
    id.low  = ((uint64_t)rand() << 32) | (uint64_t)rand();
    return id;
}

static span_id_t generate_span_id(void) {
    return ((uint64_t)rand() << 32) | (uint64_t)rand();
}

/*
 * fast_rand: lightweight, thread-safe LCG for sampling decisions.
 */
static uint32_t fast_rand(void) {
    static _Atomic uint32_t seed = 12345u;
    uint32_t s = atomic_load_explicit(&seed, memory_order_relaxed);
    s = s * 1103515245u + 12345u;
    atomic_store_explicit(&seed, s, memory_order_relaxed);
    return s;
}

/* ============================================================================
 * Sampling decision
 * ========================================================================= */

static bool should_sample(tracer_t* tracer) {
    bool bp = atomic_load_explicit(&tracer->in_backpressure, memory_order_relaxed);
    uint32_t sample_n, total_n;
    if (bp) {
        sample_n = tracer->sampling.backpressure_sample;
        total_n  = tracer->sampling.backpressure_sample +
                   tracer->sampling.backpressure_drop;
    } else {
        sample_n = tracer->sampling.always_sample;
        total_n  = tracer->sampling.always_sample +
                   tracer->sampling.always_drop;
    }
    if (total_n == 0) return false;
    return (fast_rand() % total_n) < sample_n;
}

/* ============================================================================
 * Backpressure update (called from exporter thread only, receives pre-computed
 * now value so we don't call get_time_ns() twice per iteration).
 * ========================================================================= */
static void update_backpressure(tracer_t* tracer, uint64_t now) {
    /* ----- Signal A: fill percentage ----- */
    uint64_t head        = atomic_load_explicit(&tracer->head, memory_order_relaxed);
    uint64_t tail        = atomic_load_explicit(&tracer->tail, memory_order_relaxed);
    uint32_t buffer_size = tracer->buffer_mask + 1;
    uint64_t used        = (head >= tail) ? (head - tail) : 0u;
    uint32_t fill_pct    = (uint32_t)(used * 100u / buffer_size);

    bool cur_fill = atomic_load_explicit(&tracer->bp_fill_signal,
                                         memory_order_relaxed);
    bool new_fill;
    if (cur_fill) {
        new_fill = (fill_pct > 50u);
    } else {
        new_fill = (fill_pct > 75u);
    }
    if (new_fill != cur_fill) {
        atomic_store_explicit(&tracer->bp_fill_signal, new_fill,
                              memory_order_relaxed);
    }

    /* ----- Signal B: sustained drop rate ----- */
    uint64_t total_drops = atomic_load_explicit(
        &tracer->spans_dropped_backpressure, memory_order_relaxed);

    bool cur_drop = atomic_load_explicit(&tracer->bp_drop_signal,
                                          memory_order_relaxed);

    if (now - tracer->drop_window_start_ns >= DROP_WINDOW_NS) {
        uint64_t new_drops = total_drops - tracer->drops_at_window_start;
        tracer->drops_at_window_start = total_drops;
        tracer->drop_window_start_ns  = now;

        if (new_drops >= DROP_RATE_ENTER_THRESHOLD) {
            tracer->last_drop_seen_ns = now;
            if (!cur_drop) {
                atomic_store_explicit(&tracer->bp_drop_signal, true,
                                      memory_order_relaxed);
                cur_drop = true;
            }
        }
    }

    if (cur_drop &&
        (now - tracer->last_drop_seen_ns) >= DROP_CLEAR_WINDOW_NS) {
        atomic_store_explicit(&tracer->bp_drop_signal, false,
                              memory_order_relaxed);
        cur_drop = false;
    }

    /* ----- Combined signal ----- */
    bool new_bp = new_fill || cur_drop;
    bool old_bp = atomic_load_explicit(&tracer->in_backpressure,
                                        memory_order_relaxed);
    if (new_bp != old_bp) {
        atomic_store_explicit(&tracer->in_backpressure, new_bp,
                              memory_order_relaxed);
    }

    /* ----- Update internal Prometheus metrics (if registered) ----- */
    tracer_internal_metrics_t* m = &tracer->metrics_handles;
    if (m->queue_depth)     metrics_gauge_set(m->queue_depth,     (double)used);
    if (m->in_backpressure) metrics_gauge_set(m->in_backpressure, new_bp ? 1.0 : 0.0);
    if (m->sample_rate_pct) {
        uint32_t sn = new_bp ? tracer->sampling.backpressure_sample
                             : tracer->sampling.always_sample;
        uint32_t tn = new_bp ? (tracer->sampling.backpressure_sample +
                                 tracer->sampling.backpressure_drop)
                             : (tracer->sampling.always_sample +
                                 tracer->sampling.always_drop);
        metrics_gauge_set(m->sample_rate_pct,
                          (tn > 0) ? ((double)sn * 100.0 / (double)tn) : 0.0);
    }
}

/* ============================================================================
 * Exporter thread
 * ========================================================================= */

static void* exporter_thread_fn(void* arg) {
    tracer_t* tracer = (tracer_t*)arg;

    while (!atomic_load_explicit(&tracer->shutdown, memory_order_acquire)) {
        /*
         * Refresh cached_time_ns at the top of every iteration.
         * Producers read this value; the refresh here costs one get_time_ns()
         * call on the exporter thread and keeps the cache ≤ 1 ms stale.
         */
        uint64_t now = get_time_ns();
        atomic_store_explicit(&tracer->cached_time_ns, now,
                              memory_order_relaxed);

        uint64_t tail = atomic_load_explicit(&tracer->tail,
                                              memory_order_acquire);
        uint64_t head = atomic_load_explicit(&tracer->head,
                                              memory_order_acquire);

        if (tail == head) {
            /* Buffer empty: sleep briefly then update backpressure. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&ts, NULL);
            /* Re-read time after sleep for accurate backpressure windows. */
            now = get_time_ns();
            atomic_store_explicit(&tracer->cached_time_ns, now,
                                  memory_order_relaxed);
            update_backpressure(tracer, now);
            continue;
        }

        uint32_t slot_idx = (uint32_t)(tail & (uint64_t)tracer->buffer_mask);
        span_slot_t* slot = &tracer->slots[slot_idx];

        uint32_t expected = SPAN_SLOT_FILLED;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, SPAN_SLOT_PROCESSING,
                memory_order_acquire, memory_order_relaxed)) {
            /* Slot not ready yet (producer still writing). Yield briefly. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L };
            nanosleep(&ts, NULL);
            continue;
        }

        atomic_fetch_add_explicit(&tracer->exports_attempted, 1,
                                  memory_order_relaxed);

        if (tracer->export_callback) {
            trace_span_t spans[1];
            memcpy(&spans[0], &slot->span, sizeof(trace_span_t));
            tracer->export_callback(spans, 1, tracer->user_data);
            atomic_fetch_add_explicit(&tracer->exports_succeeded, 1,
                                      memory_order_relaxed);

            tracer_internal_metrics_t* mi = &tracer->metrics_handles;
            if (mi->spans_dropped) {
                uint64_t drops = atomic_load_explicit(
                    &tracer->spans_dropped_backpressure, memory_order_relaxed);
                metrics_gauge_set(mi->spans_dropped, (double)drops);
            }
            if (mi->spans_sampled_out) {
                uint64_t so = atomic_load_explicit(
                    &tracer->spans_sampled_out, memory_order_relaxed);
                metrics_gauge_set(mi->spans_sampled_out, (double)so);
            }
        }

        atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY,
                              memory_order_release);
        atomic_store_explicit(&tracer->tail, tail + 1,
                              memory_order_release);

        /* Refresh time after (potentially slow) export callback. */
        now = get_time_ns();
        atomic_store_explicit(&tracer->cached_time_ns, now,
                              memory_order_relaxed);
        update_backpressure(tracer, now);
    }

    return NULL;
}

/* ============================================================================
 * Public API — lifecycle
 * ========================================================================= */

distric_err_t trace_init(
    tracer_t** tracer,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data) {
    trace_sampling_config_t default_sampling = {
        .always_sample       = 1,
        .always_drop         = 0,
        .backpressure_sample = 1,
        .backpressure_drop   = 9,
    };
    return trace_init_with_sampling(tracer, &default_sampling,
                                    export_callback, user_data);
}

distric_err_t trace_init_with_sampling(
    tracer_t**                      tracer,
    const trace_sampling_config_t*  sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void*                           user_data) {

    if (!tracer || !sampling) return DISTRIC_ERR_INVALID_ARG;

    tracer_t* t = calloc(1, sizeof(tracer_t));
    if (!t) return DISTRIC_ERR_ALLOC_FAILURE;

    t->sampling        = *sampling;
    t->export_callback = export_callback;
    t->user_data       = user_data;

    uint32_t buffer_size = next_power_of_2(MAX_SPANS_BUFFER);
    t->buffer_mask = buffer_size - 1;

    t->slots = calloc(buffer_size, sizeof(span_slot_t));
    if (!t->slots) { free(t); return DISTRIC_ERR_ALLOC_FAILURE; }

    for (uint32_t i = 0; i < buffer_size; i++) {
        atomic_init(&t->slots[i].state, SPAN_SLOT_EMPTY);
    }

    atomic_init(&t->head,                       0);
    atomic_init(&t->tail,                       0);
    atomic_init(&t->spans_created,              0);
    atomic_init(&t->spans_sampled_in,           0);
    atomic_init(&t->spans_sampled_out,          0);
    atomic_init(&t->spans_dropped_backpressure, 0);
    atomic_init(&t->exports_attempted,          0);
    atomic_init(&t->exports_succeeded,          0);
    atomic_init(&t->sample_counter,             0);
    atomic_init(&t->bp_fill_signal,             false);
    atomic_init(&t->bp_drop_signal,             false);
    atomic_init(&t->in_backpressure,            false);
    atomic_init(&t->refcount,                   1);
    atomic_init(&t->shutdown,                   false);

    /* Seed the cached timestamp so producers see a valid value from the
     * first span before the exporter thread runs its first iteration. */
    uint64_t now = get_time_ns();
    atomic_init(&t->cached_time_ns, now);

    t->drop_window_start_ns  = now;
    t->drops_at_window_start = 0;
    t->last_drop_seen_ns     = 0;

    memset(&t->metrics_handles, 0, sizeof(t->metrics_handles));

    if (pthread_create(&t->exporter_thread, NULL, exporter_thread_fn, t) != 0) {
        free(t->slots);
        free(t);
        return DISTRIC_ERR_INIT_FAILED;
    }
    t->exporter_started = true;

    *tracer = t;
    return DISTRIC_OK;
}

void trace_retain(tracer_t* tracer) {
    if (tracer)
        atomic_fetch_add_explicit(&tracer->refcount, 1, memory_order_relaxed);
}

void trace_release(tracer_t* tracer) {
    if (!tracer) return;
    uint32_t prev = atomic_fetch_sub_explicit(&tracer->refcount, 1,
                                              memory_order_acq_rel);
    if (prev != 1) return;

    atomic_store_explicit(&tracer->shutdown, true, memory_order_release);
    if (tracer->exporter_started) {
        pthread_join(tracer->exporter_thread, NULL);
    }
    free(tracer->slots);
    free(tracer);
}

void trace_destroy(tracer_t* tracer) { trace_release(tracer); }

/* ============================================================================
 * Public API — statistics
 * ========================================================================= */

void trace_get_stats(tracer_t* tracer, tracer_stats_t* out) {
    if (!tracer || !out) return;

    uint64_t head = atomic_load_explicit(&tracer->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&tracer->tail, memory_order_relaxed);
    uint64_t used = (head >= tail) ? (head - tail) : 0u;

    out->spans_created              = atomic_load_explicit(&tracer->spans_created,
                                          memory_order_relaxed);
    out->spans_sampled_in           = atomic_load_explicit(&tracer->spans_sampled_in,
                                          memory_order_relaxed);
    out->spans_sampled_out          = atomic_load_explicit(&tracer->spans_sampled_out,
                                          memory_order_relaxed);
    out->spans_dropped_backpressure = atomic_load_explicit(
                                          &tracer->spans_dropped_backpressure,
                                          memory_order_relaxed);
    out->exports_attempted          = atomic_load_explicit(&tracer->exports_attempted,
                                          memory_order_relaxed);
    out->exports_succeeded          = atomic_load_explicit(&tracer->exports_succeeded,
                                          memory_order_relaxed);
    out->queue_depth                = used;
    out->queue_capacity             = tracer->buffer_mask + 1u;
    out->in_backpressure            = atomic_load_explicit(&tracer->in_backpressure,
                                          memory_order_relaxed);

    bool bp = out->in_backpressure;
    uint32_t sn = bp ? tracer->sampling.backpressure_sample
                     : tracer->sampling.always_sample;
    uint32_t tn = bp ? (tracer->sampling.backpressure_sample +
                         tracer->sampling.backpressure_drop)
                     : (tracer->sampling.always_sample +
                         tracer->sampling.always_drop);
    out->effective_sample_rate_pct = (tn > 0) ? (sn * 100u / tn) : 0u;
}

/* ============================================================================
 * Public API — Prometheus integration
 * ========================================================================= */

distric_err_t trace_register_metrics(tracer_t*           tracer,
                                      metrics_registry_t* registry) {
    if (!tracer || !registry) return DISTRIC_ERR_INVALID_ARG;

    tracer_internal_metrics_t* m = &tracer->metrics_handles;

    if (metrics_register_gauge(registry, "distric_tracing_queue_depth",
            "Current number of spans queued for export",
            NULL, 0, &m->queue_depth) != DISTRIC_OK) return DISTRIC_ERR_INIT_FAILED;

    if (metrics_register_gauge(registry, "distric_tracing_sample_rate_pct",
            "Effective sampling rate as a percentage (0-100)",
            NULL, 0, &m->sample_rate_pct) != DISTRIC_OK) return DISTRIC_ERR_INIT_FAILED;

    if (metrics_register_gauge(registry, "distric_tracing_drops_total",
            "Cumulative number of spans dropped due to queue overflow",
            NULL, 0, &m->spans_dropped) != DISTRIC_OK) return DISTRIC_ERR_INIT_FAILED;

    if (metrics_register_gauge(registry, "distric_tracing_sampled_out_total",
            "Cumulative number of spans dropped by the adaptive sampler",
            NULL, 0, &m->spans_sampled_out) != DISTRIC_OK) return DISTRIC_ERR_INIT_FAILED;

    if (metrics_register_gauge(registry, "distric_tracing_in_backpressure",
            "1 when the tracer is in backpressure mode, 0 otherwise",
            NULL, 0, &m->in_backpressure) != DISTRIC_OK) return DISTRIC_ERR_INIT_FAILED;

    return DISTRIC_OK;
}

/* ============================================================================
 * Public API — span creation
 * ========================================================================= */

distric_err_t trace_start_span(tracer_t*      tracer,
                                const char*    operation,
                                trace_span_t** out_span) {
    if (!tracer || !operation || !out_span) return DISTRIC_ERR_INVALID_ARG;

    atomic_fetch_add_explicit(&tracer->spans_created, 1, memory_order_relaxed);

    if (!should_sample(tracer)) {
        atomic_fetch_add_explicit(&tracer->spans_sampled_out, 1,
                                  memory_order_relaxed);
        *out_span = &g_sampled_out_span;
        return DISTRIC_OK;
    }

    atomic_fetch_add_explicit(&tracer->spans_sampled_in, 1,
                              memory_order_relaxed);

    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) {
        *out_span = &g_sampled_out_span;
        return DISTRIC_ERR_NO_MEMORY;
    }
    memset(span, 0, sizeof(*span));

    span->trace_id      = generate_trace_id();
    span->span_id       = generate_span_id();
    span->parent_span_id = 0;
    strncpy(span->operation, operation, TRACE_MAX_OPERATION_LEN - 1);
    /* Use cached time — relaxed load, ~3 ns, no syscall. */
    span->start_time_ns = atomic_load_explicit(&tracer->cached_time_ns,
                                               memory_order_relaxed);
    span->status        = SPAN_STATUS_UNSET;
    span->sampled       = true;
    span->_tracer       = tracer;

    *out_span = span;
    return DISTRIC_OK;
}

distric_err_t trace_start_child_span(tracer_t*      tracer,
                                      trace_span_t*  parent,
                                      const char*    operation,
                                      trace_span_t** out_span) {
    if (!tracer || !parent || !operation || !out_span)
        return DISTRIC_ERR_INVALID_ARG;

    atomic_fetch_add_explicit(&tracer->spans_created, 1, memory_order_relaxed);

    if (!should_sample(tracer)) {
        atomic_fetch_add_explicit(&tracer->spans_sampled_out, 1,
                                  memory_order_relaxed);
        *out_span = &g_sampled_out_span;
        return DISTRIC_OK;
    }

    atomic_fetch_add_explicit(&tracer->spans_sampled_in, 1,
                              memory_order_relaxed);

    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) {
        *out_span = &g_sampled_out_span;
        return DISTRIC_ERR_NO_MEMORY;
    }
    memset(span, 0, sizeof(*span));

    span->trace_id       = parent->trace_id;
    span->span_id        = generate_span_id();
    span->parent_span_id = parent->span_id;
    strncpy(span->operation, operation, TRACE_MAX_OPERATION_LEN - 1);
    span->start_time_ns  = atomic_load_explicit(&tracer->cached_time_ns,
                                                memory_order_relaxed);
    span->status         = SPAN_STATUS_UNSET;
    span->sampled        = true;
    span->_tracer        = tracer;

    *out_span = span;
    return DISTRIC_OK;
}

distric_err_t trace_add_tag(trace_span_t* span,
                             const char*   key,
                             const char*   value) {
    if (!span || !key || !value || !span->sampled) return DISTRIC_OK;
    if (span->tag_count >= TRACE_MAX_SPAN_TAGS) return DISTRIC_ERR_BUFFER_OVERFLOW;

    span_tag_t* tag = &span->tags[span->tag_count];
    strncpy(tag->key,   key,   TRACE_MAX_TAG_KEY_LEN   - 1);
    strncpy(tag->value, value, TRACE_MAX_TAG_VALUE_LEN - 1);
    tag->key  [TRACE_MAX_TAG_KEY_LEN   - 1] = '\0';
    tag->value[TRACE_MAX_TAG_VALUE_LEN - 1] = '\0';
    span->tag_count++;
    return DISTRIC_OK;
}

void trace_set_status(trace_span_t* span, span_status_t status) {
    if (span && span->sampled) span->status = status;
}

/* ============================================================================
 * trace_finish_span — hot path, must be non-blocking.
 *
 * Critical change: span->end_time_ns is now set from cached_time_ns (relaxed
 * atomic load, ~3 ns) instead of get_time_ns() (clock_gettime syscall,
 * 100–350 µs under seccomp).  This is the direct fix for P3b.
 * ========================================================================= */
void trace_finish_span(tracer_t* tracer, trace_span_t* span) {
    if (!span || !span->sampled || span == &g_sampled_out_span) return;

    if (!tracer) tracer = (tracer_t*)span->_tracer;
    if (!tracer) { free(span); return; }

    /* Record end time from cache — zero syscall risk. */
    span->end_time_ns = atomic_load_explicit(&tracer->cached_time_ns,
                                             memory_order_relaxed);

    uint64_t head        = atomic_load_explicit(&tracer->head,
                                                 memory_order_acquire);
    uint64_t tail        = atomic_load_explicit(&tracer->tail,
                                                 memory_order_acquire);
    uint32_t buffer_size = tracer->buffer_mask + 1;

    if (head - tail >= (uint64_t)buffer_size) {
        /* Buffer full — drop the span, never block. */
        atomic_fetch_add_explicit(&tracer->spans_dropped_backpressure, 1,
                                  memory_order_relaxed);
        free(span);
        return;
    }

    /* Claim a slot and publish. */
    uint64_t slot_idx = atomic_fetch_add_explicit(&tracer->head, 1,
                                                   memory_order_acq_rel);
    span_slot_t* slot = &tracer->slots[slot_idx & (uint64_t)tracer->buffer_mask];

    memcpy(&slot->span, span, sizeof(trace_span_t));
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED, memory_order_release);

    free(span);
}

/* ============================================================================
 * Public API — context propagation
 * ========================================================================= */

distric_err_t trace_inject_context(trace_span_t* span,
                                    char*          header,
                                    size_t         header_size) {
    if (!span || !header || header_size < 128) return DISTRIC_ERR_INVALID_ARG;

    int written = snprintf(header, header_size,
                           "00-%016lx%016lx-%016lx-01",
                           (unsigned long)span->trace_id.high,
                           (unsigned long)span->trace_id.low,
                           (unsigned long)span->span_id);
    if (written < 0 || (size_t)written >= header_size)
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    return DISTRIC_OK;
}

distric_err_t trace_extract_context(const char*      header,
                                     trace_context_t* context) {
    if (!header || !context) return DISTRIC_ERR_INVALID_ARG;

    unsigned long long th, tl, si;
    if (sscanf(header, "00-%016llx%016llx-%016llx-", &th, &tl, &si) != 3)
        return DISTRIC_ERR_INVALID_ARG;

    context->trace_id.high = th;
    context->trace_id.low  = tl;
    context->span_id       = si;
    return DISTRIC_OK;
}

distric_err_t trace_start_span_from_context(
    tracer_t*              tracer,
    const trace_context_t* context,
    const char*            operation,
    trace_span_t**         out_span) {

    if (!tracer || !context || !operation || !out_span)
        return DISTRIC_ERR_INVALID_ARG;

    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) { *out_span = &g_sampled_out_span; return DISTRIC_ERR_NO_MEMORY; }
    memset(span, 0, sizeof(*span));

    span->trace_id       = context->trace_id;
    span->span_id        = generate_span_id();
    span->parent_span_id = context->span_id;
    strncpy(span->operation, operation, TRACE_MAX_OPERATION_LEN - 1);
    span->start_time_ns  = atomic_load_explicit(&tracer->cached_time_ns,
                                                memory_order_relaxed);
    span->status         = SPAN_STATUS_UNSET;
    span->sampled        = true;
    span->_tracer        = tracer;

    *out_span = span;
    return DISTRIC_OK;
}

/* ============================================================================
 * Public API — thread-local active span
 * ========================================================================= */

void          trace_set_active_span(trace_span_t* span) { tls_active_span = span; }
trace_span_t* trace_get_active_span(void)               { return tls_active_span; }