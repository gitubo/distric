/*
 * tracing.c — DistriC Observability Library — Tracing Implementation
 *
 * =============================================================================
 * ADAPTIVE SAMPLING — PRODUCTION BLOCKER #2
 * =============================================================================
 *
 * Two-signal backpressure model:
 *
 *   Signal A — queue fill:
 *     Enter backpressure mode when buffer > 75% full.
 *     Exit  when buffer < 50% full (hysteresis).
 *
 *   Signal B — sustained drop rate:
 *     Enter when >= DROP_RATE_ENTER_THRESHOLD spans dropped in one second.
 *     Exit after DROP_CLEAR_WINDOW_NS of zero new drops.
 *
 *   in_backpressure = Signal A || Signal B
 *
 * Sampling rates applied atomically:
 *   Normal:       always_sample / (always_sample + always_drop)
 *   Backpressure: backpressure_sample / (backpressure_sample + backpressure_drop)
 *
 * Hot-path (trace_start_span / trace_finish_span) never calls clock_gettime().
 * The exporter thread refreshes cached_time_ns every iteration (< 1 ms stale).
 *
 * Thread-safety:
 *   - trace_start_span / trace_finish_span: lock-free; safe from any thread.
 *   - update_backpressure: called only from exporter thread.
 *   - trace_get_stats / trace_register_metrics: safe from any thread.
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
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

/* ============================================================================
 * Internal constants
 * ========================================================================= */

#define SPAN_SLOT_EMPTY      0u
#define SPAN_SLOT_FILLED     1u
#define SPAN_SLOT_PROCESSING 2u

/* Backpressure thresholds */
#define BP_FILL_ENTER_PCT       75u
#define BP_FILL_EXIT_PCT        50u
#define DROP_WINDOW_NS          1000000000ULL  /* 1 s  */
#define DROP_RATE_ENTER_THRESHOLD 5u
#define DROP_CLEAR_WINDOW_NS    3000000000ULL  /* 3 s  */

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
    span_slot_t*     slots;
    uint32_t         buffer_mask;

    _Atomic uint64_t head;
    _Atomic uint64_t tail;

    trace_sampling_config_t sampling;

    /* Backpressure state */
    _Atomic bool     bp_fill_signal;
    _Atomic bool     bp_drop_signal;
    _Atomic bool     in_backpressure;

    /* Drop-rate window (written only from exporter thread) */
    uint64_t         drop_window_start_ns;
    uint64_t         drops_at_window_start;
    uint64_t         last_drop_seen_ns;

    /*
     * cached_time_ns: CLOCK_MONOTONIC ns, refreshed by exporter thread.
     * Producers use this via relaxed load (~3 ns) to avoid syscall latency.
     */
    _Atomic uint64_t cached_time_ns;

    /* Counters */
    _Atomic uint64_t spans_created;
    _Atomic uint64_t spans_sampled_in;
    _Atomic uint64_t spans_sampled_out;
    _Atomic uint64_t spans_dropped_backpressure;
    _Atomic uint64_t exports_attempted;
    _Atomic uint64_t exports_succeeded;
    _Atomic uint64_t sample_counter;

    /* Export */
    void (*export_callback)(trace_span_t*, size_t, void*);
    void* user_data;

    /* Lifecycle */
    _Atomic uint32_t refcount;
    _Atomic bool     shutdown;
    pthread_t        exporter_thread;
    bool             exporter_started;

    /* Optional Prometheus integration */
    tracer_internal_metrics_t metrics_handles;
};

/* Global no-op span for sampled-out spans. */
static trace_span_t g_sampled_out_span = { .sampled = false };

/* Thread-local active span. */
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

/* Direct clock read — only called from exporter thread or init. */
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

static uint32_t fast_rand(void) {
    static _Atomic uint32_t seed = 12345u;
    uint32_t s = atomic_load_explicit(&seed, memory_order_relaxed);
    s = s * 1103515245u + 12345u;
    atomic_store_explicit(&seed, s, memory_order_relaxed);
    return s;
}

/* ============================================================================
 * Sampling decision — hot path
 * ========================================================================= */

static bool should_sample(tracer_t* t) {
    bool bp = atomic_load_explicit(&t->in_backpressure, memory_order_relaxed);
    uint32_t sn, tn;
    if (bp) {
        sn = t->sampling.backpressure_sample;
        tn = t->sampling.backpressure_sample + t->sampling.backpressure_drop;
    } else {
        sn = t->sampling.always_sample;
        tn = t->sampling.always_sample + t->sampling.always_drop;
    }
    if (tn == 0) return false;
    return (fast_rand() % tn) < sn;
}

/* ============================================================================
 * Backpressure update — exporter thread only
 * ========================================================================= */

static void update_backpressure(tracer_t* t, uint64_t now) {
    /* Signal A: fill percentage with hysteresis */
    uint64_t head  = atomic_load_explicit(&t->head, memory_order_relaxed);
    uint64_t tail  = atomic_load_explicit(&t->tail, memory_order_relaxed);
    uint32_t bsz   = t->buffer_mask + 1;
    uint64_t used  = (head >= tail) ? (head - tail) : 0u;
    uint32_t fpct  = (uint32_t)(used * 100u / bsz);

    bool cur_fill = atomic_load_explicit(&t->bp_fill_signal, memory_order_relaxed);
    bool new_fill = cur_fill ? (fpct > BP_FILL_EXIT_PCT) : (fpct > BP_FILL_ENTER_PCT);
    if (new_fill != cur_fill)
        atomic_store_explicit(&t->bp_fill_signal, new_fill, memory_order_relaxed);

    /* Signal B: sustained drop rate */
    uint64_t total_drops = atomic_load_explicit(
        &t->spans_dropped_backpressure, memory_order_relaxed);
    bool cur_drop = atomic_load_explicit(&t->bp_drop_signal, memory_order_relaxed);

    if (now - t->drop_window_start_ns >= DROP_WINDOW_NS) {
        uint64_t new_drops = total_drops - t->drops_at_window_start;
        t->drops_at_window_start = total_drops;
        t->drop_window_start_ns  = now;
        if (new_drops >= DROP_RATE_ENTER_THRESHOLD) {
            t->last_drop_seen_ns = now;
            if (!cur_drop) {
                atomic_store_explicit(&t->bp_drop_signal, true, memory_order_relaxed);
                cur_drop = true;
            }
        }
    }
    if (cur_drop && (now - t->last_drop_seen_ns) >= DROP_CLEAR_WINDOW_NS) {
        atomic_store_explicit(&t->bp_drop_signal, false, memory_order_relaxed);
        cur_drop = false;
    }

    /* Combined */
    bool new_bp = new_fill || cur_drop;
    bool old_bp = atomic_load_explicit(&t->in_backpressure, memory_order_relaxed);
    if (new_bp != old_bp)
        atomic_store_explicit(&t->in_backpressure, new_bp, memory_order_relaxed);

    /* Update Prometheus gauges if registered */
    tracer_internal_metrics_t* m = &t->metrics_handles;
    if (m->queue_depth)     metrics_gauge_set(m->queue_depth,     (double)used);
    if (m->in_backpressure) metrics_gauge_set(m->in_backpressure, new_bp ? 1.0 : 0.0);
    if (m->spans_dropped) {
        metrics_gauge_set(m->spans_dropped,
            (double)atomic_load_explicit(&t->spans_dropped_backpressure,
                                         memory_order_relaxed));
    }
    if (m->spans_sampled_out) {
        metrics_gauge_set(m->spans_sampled_out,
            (double)atomic_load_explicit(&t->spans_sampled_out,
                                         memory_order_relaxed));
    }
    if (m->sample_rate_pct) {
        uint32_t sn = new_bp ? t->sampling.backpressure_sample
                              : t->sampling.always_sample;
        uint32_t tn = new_bp ? (t->sampling.backpressure_sample +
                                 t->sampling.backpressure_drop)
                              : (t->sampling.always_sample +
                                 t->sampling.always_drop);
        metrics_gauge_set(m->sample_rate_pct,
                          (tn > 0) ? ((double)sn * 100.0 / (double)tn) : 0.0);
    }
}

/* ============================================================================
 * Exporter thread
 * ========================================================================= */

static void* exporter_thread_fn(void* arg) {
    tracer_t* t = (tracer_t*)arg;

    while (!atomic_load_explicit(&t->shutdown, memory_order_acquire)) {
        uint64_t now = get_time_ns();
        atomic_store_explicit(&t->cached_time_ns, now, memory_order_relaxed);

        uint64_t tail = atomic_load_explicit(&t->tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&t->head, memory_order_acquire);

        if (tail == head) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&ts, NULL);
            now = get_time_ns();
            atomic_store_explicit(&t->cached_time_ns, now, memory_order_relaxed);
            update_backpressure(t, now);
            continue;
        }

        uint32_t slot_idx = (uint32_t)(tail & (uint64_t)t->buffer_mask);
        span_slot_t* slot = &t->slots[slot_idx];

        uint32_t expected = SPAN_SLOT_FILLED;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, SPAN_SLOT_PROCESSING,
                memory_order_acquire, memory_order_relaxed)) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000L };
            nanosleep(&ts, NULL);
            continue;
        }

        atomic_fetch_add_explicit(&t->exports_attempted, 1, memory_order_relaxed);

        if (t->export_callback) {
            trace_span_t spans[1];
            memcpy(&spans[0], &slot->span, sizeof(trace_span_t));
            t->export_callback(spans, 1, t->user_data);
            atomic_fetch_add_explicit(&t->exports_succeeded, 1, memory_order_relaxed);
        }

        atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY, memory_order_release);
        atomic_store_explicit(&t->tail, tail + 1, memory_order_release);

        now = get_time_ns();
        atomic_store_explicit(&t->cached_time_ns, now, memory_order_relaxed);
        update_backpressure(t, now);
    }
    return NULL;
}

/* ============================================================================
 * Lifecycle
 * ========================================================================= */

distric_err_t trace_init(tracer_t** tracer,
                          void (*export_cb)(trace_span_t*, size_t, void*),
                          void* user_data) {
    trace_sampling_config_t def = {
        .always_sample       = 1,
        .always_drop         = 0,
        .backpressure_sample = 1,
        .backpressure_drop   = 9,
    };
    return trace_init_with_sampling(tracer, &def, export_cb, user_data);
}

distric_err_t trace_init_with_sampling(tracer_t** tracer,
                                        const trace_sampling_config_t* sampling,
                                        void (*export_cb)(trace_span_t*, size_t, void*),
                                        void* user_data) {
    if (!tracer || !sampling) return DISTRIC_ERR_INVALID_ARG;

    tracer_t* t = calloc(1, sizeof(tracer_t));
    if (!t) return DISTRIC_ERR_ALLOC_FAILURE;

    t->sampling        = *sampling;
    t->export_callback = export_cb;
    t->user_data       = user_data;

    uint32_t bsz = next_power_of_2(MAX_SPANS_BUFFER);
    t->buffer_mask = bsz - 1;
    t->slots = calloc(bsz, sizeof(span_slot_t));
    if (!t->slots) { free(t); return DISTRIC_ERR_ALLOC_FAILURE; }
    for (uint32_t i = 0; i < bsz; i++)
        atomic_init(&t->slots[i].state, SPAN_SLOT_EMPTY);

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
    atomic_init(&t->cached_time_ns,             get_time_ns());

    t->drop_window_start_ns  = get_time_ns();
    t->drops_at_window_start = 0;
    t->last_drop_seen_ns     = 0;

    memset(&t->metrics_handles, 0, sizeof(t->metrics_handles));

    if (pthread_create(&t->exporter_thread, NULL, exporter_thread_fn, t) != 0) {
        free(t->slots);
        free(t);
        return DISTRIC_ERR_THREAD;
    }
    t->exporter_started = true;

    *tracer = t;
    return DISTRIC_OK;
}

void trace_retain(tracer_t* t) {
    if (t) atomic_fetch_add_explicit(&t->refcount, 1, memory_order_relaxed);
}

void trace_release(tracer_t* t) {
    if (!t) return;
    if (atomic_fetch_sub_explicit(&t->refcount, 1, memory_order_acq_rel) != 1)
        return;
    free(t->slots);
    free(t);
}

void trace_destroy(tracer_t* t) {
    if (!t) return;
    atomic_store_explicit(&t->shutdown, true, memory_order_release);
    if (t->exporter_started)
        pthread_join(t->exporter_thread, NULL);
    trace_release(t);
}

/* ============================================================================
 * Stats
 * ========================================================================= */

void trace_get_stats(tracer_t* t, tracer_stats_t* out) {
    if (!t || !out) return;
    out->spans_created              = atomic_load_explicit(&t->spans_created,              memory_order_relaxed);
    out->spans_sampled_in           = atomic_load_explicit(&t->spans_sampled_in,           memory_order_relaxed);
    out->spans_sampled_out          = atomic_load_explicit(&t->spans_sampled_out,          memory_order_relaxed);
    out->spans_dropped_backpressure = atomic_load_explicit(&t->spans_dropped_backpressure, memory_order_relaxed);
    out->exports_attempted          = atomic_load_explicit(&t->exports_attempted,          memory_order_relaxed);
    out->exports_succeeded          = atomic_load_explicit(&t->exports_succeeded,          memory_order_relaxed);
    out->in_backpressure            = atomic_load_explicit(&t->in_backpressure,            memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&t->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&t->tail, memory_order_relaxed);
    out->queue_depth    = (head >= tail) ? (head - tail) : 0;
    out->queue_capacity = t->buffer_mask + 1;
    bool bp = out->in_backpressure;
    uint32_t sn = bp ? t->sampling.backpressure_sample : t->sampling.always_sample;
    uint32_t tn = bp ? (t->sampling.backpressure_sample + t->sampling.backpressure_drop)
                     : (t->sampling.always_sample       + t->sampling.always_drop);
    out->effective_sample_rate_pct = (tn > 0) ? (sn * 100u / tn) : 0u;
}

distric_err_t trace_register_metrics(tracer_t* t, metrics_registry_t* registry) {
    if (!t || !registry) return DISTRIC_ERR_INVALID_ARG;
    tracer_internal_metrics_t* m = &t->metrics_handles;
    if (metrics_register_gauge(registry, "distric_tracing_queue_depth",
            "Current spans queued for export",
            NULL, 0, &m->queue_depth) != DISTRIC_OK)
        return DISTRIC_ERR_INIT_FAILED;
    if (metrics_register_gauge(registry, "distric_tracing_sample_rate_pct",
            "Effective sampling rate as a percentage (0-100)",
            NULL, 0, &m->sample_rate_pct) != DISTRIC_OK)
        return DISTRIC_ERR_INIT_FAILED;
    if (metrics_register_gauge(registry, "distric_tracing_drops_total",
            "Cumulative spans dropped due to queue overflow",
            NULL, 0, &m->spans_dropped) != DISTRIC_OK)
        return DISTRIC_ERR_INIT_FAILED;
    if (metrics_register_gauge(registry, "distric_tracing_sampled_out_total",
            "Cumulative spans dropped by adaptive sampler",
            NULL, 0, &m->spans_sampled_out) != DISTRIC_OK)
        return DISTRIC_ERR_INIT_FAILED;
    if (metrics_register_gauge(registry, "distric_tracing_in_backpressure",
            "1 when in backpressure mode, 0 otherwise",
            NULL, 0, &m->in_backpressure) != DISTRIC_OK)
        return DISTRIC_ERR_INIT_FAILED;
    return DISTRIC_OK;
}

/* ============================================================================
 * Span creation
 * ========================================================================= */

distric_err_t trace_start_span(tracer_t* t, const char* op, trace_span_t** out) {
    if (!t || !op || !out) return DISTRIC_ERR_INVALID_ARG;
    atomic_fetch_add_explicit(&t->spans_created, 1, memory_order_relaxed);

    if (!should_sample(t)) {
        atomic_fetch_add_explicit(&t->spans_sampled_out, 1, memory_order_relaxed);
        *out = &g_sampled_out_span;
        return DISTRIC_OK;
    }
    atomic_fetch_add_explicit(&t->spans_sampled_in, 1, memory_order_relaxed);

    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) { *out = &g_sampled_out_span; return DISTRIC_ERR_NO_MEMORY; }
    memset(span, 0, sizeof(*span));
    span->trace_id       = generate_trace_id();
    span->span_id        = generate_span_id();
    span->parent_span_id = 0;
    strncpy(span->operation, op, TRACE_MAX_OPERATION_LEN - 1);
    span->start_time_ns  = atomic_load_explicit(&t->cached_time_ns, memory_order_relaxed);
    span->sampled        = true;
    span->_tracer        = t;
    tls_active_span      = span;
    *out = span;
    return DISTRIC_OK;
}

distric_err_t trace_start_child_span(tracer_t* t, trace_span_t* parent,
                                      const char* op, trace_span_t** out) {
    if (!t || !op || !out) return DISTRIC_ERR_INVALID_ARG;
    distric_err_t err = trace_start_span(t, op, out);
    if (err == DISTRIC_OK && *out && (*out)->sampled && parent && parent->sampled)
        (*out)->parent_span_id = parent->span_id;
    return err;
}

distric_err_t trace_start_span_from_context(tracer_t* t, const trace_context_t* ctx,
                                              const char* op, trace_span_t** out) {
    if (!t || !op || !out) return DISTRIC_ERR_INVALID_ARG;
    distric_err_t err = trace_start_span(t, op, out);
    if (err == DISTRIC_OK && *out && (*out)->sampled && ctx) {
        (*out)->trace_id       = ctx->trace_id;
        (*out)->parent_span_id = ctx->span_id;
    }
    return err;
}

/* ============================================================================
 * Span finish — lock-free enqueue
 * ========================================================================= */

distric_err_t trace_finish_span(tracer_t* t, trace_span_t* span) {
    if (!t || !span) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;  /* no-op placeholder */

    span->end_time_ns = atomic_load_explicit(&t->cached_time_ns, memory_order_relaxed);
    if (tls_active_span == span) tls_active_span = NULL;

    /* Claim a slot in the ring buffer (lock-free). */
    uint64_t head = atomic_fetch_add_explicit(&t->head, 1, memory_order_acq_rel);
    uint64_t tail = atomic_load_explicit(&t->tail, memory_order_acquire);

    if (head - tail >= (uint64_t)(t->buffer_mask + 1)) {
        /* Buffer full — drop span. */
        atomic_fetch_add_explicit(&t->spans_dropped_backpressure, 1,
                                  memory_order_relaxed);
        /* Roll back head by re-decrement so tail stays coherent. */
        atomic_fetch_sub_explicit(&t->head, 1, memory_order_relaxed);
        free(span);
        return DISTRIC_ERR_BACKPRESSURE;
    }

    uint32_t slot_idx = (uint32_t)(head & (uint64_t)t->buffer_mask);
    span_slot_t* slot = &t->slots[slot_idx];

    /* Wait for exporter to clear this slot (should be near-instant). */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SPAN_SLOT_EMPTY) {
        if (++spin > 1000) {
            /* Exporter extremely slow — drop to stay non-blocking. */
            atomic_fetch_add_explicit(&t->spans_dropped_backpressure, 1,
                                      memory_order_relaxed);
            atomic_fetch_sub_explicit(&t->head, 1, memory_order_relaxed);
            free(span);
            return DISTRIC_ERR_BACKPRESSURE;
        }
        struct timespec ts = { 0, 100 };
        nanosleep(&ts, NULL);
    }

    memcpy(&slot->span, span, sizeof(trace_span_t));
    free(span);
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED, memory_order_release);
    return DISTRIC_OK;
}

/* ============================================================================
 * Tag / status / context
 * ========================================================================= */

distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value) {
    if (!span || !key || !value) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;
    if (span->tag_count >= TRACE_MAX_SPAN_TAGS) return DISTRIC_ERR_REGISTRY_FULL;
    span_tag_t* tag = &span->tags[span->tag_count++];
    strncpy(tag->key,   key,   TRACE_MAX_TAG_KEY_LEN   - 1);
    strncpy(tag->value, value, TRACE_MAX_TAG_VALUE_LEN - 1);
    return DISTRIC_OK;
}

distric_err_t trace_set_status(trace_span_t* span, span_status_t status) {
    if (!span) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;
    span->status = (int)status;
    return DISTRIC_OK;
}

distric_err_t trace_inject_context(trace_span_t* span, char* buf, size_t buf_size) {
    if (!span || !buf || buf_size < 64) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) { buf[0] = '\0'; return DISTRIC_OK; }
    snprintf(buf, buf_size, "%016llx%016llx-%016llx",
             (unsigned long long)span->trace_id.high,
             (unsigned long long)span->trace_id.low,
             (unsigned long long)span->span_id);
    return DISTRIC_OK;
}

distric_err_t trace_extract_context(const char* header, trace_context_t* out_ctx) {
    if (!header || !out_ctx) return DISTRIC_ERR_INVALID_ARG;
    unsigned long long th, tl, sid;
    if (sscanf(header, "%016llx%016llx-%016llx", &th, &tl, &sid) != 3)
        return DISTRIC_ERR_INVALID_FORMAT;
    out_ctx->trace_id.high = (uint64_t)th;
    out_ctx->trace_id.low  = (uint64_t)tl;
    out_ctx->span_id       = (uint64_t)sid;
    return DISTRIC_OK;
}