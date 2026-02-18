/*
 * tracing.c — DistriC Observability Library — Tracing Implementation
 *
 * Architecture (Improvement #7 — layered design):
 *
 *   Layer 1 — Span buffer mechanism (span_buffer_t):
 *     Lock-free MPSC ring.  Producers claim slots via atomic fetch-add.
 *     Exporter drains filled slots.
 *
 *   Layer 2 — Sampling policy state machine (sampling_state_t):
 *     Two-signal adaptive model.  Policy is data-driven and immutable
 *     after init except for the in_backpressure flag, which is written
 *     ONLY by the exporter thread (no concurrent writers on policy state).
 *
 *   Layer 3 — Export scheduling:
 *     Background thread sleeps for export_interval_ms, drains, exports.
 *     Refreshes cached_time_ns for producers to read without syscalls.
 *
 * Hot-path invariants:
 *   - trace_start_span / trace_finish_span: lock-free; any thread.
 *   - Policy state (in_backpressure) is read relaxed on the hot path;
 *     written only by the exporter thread.
 *   - cached_time_ns is ≤ export_interval_ms stale — acceptable for spans.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include "distric_obs/tracing.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <assert.h>

/* ============================================================================
 * Global no-op span — returned when a span is sampled out.
 * Caller writes into it freely; writes are benign (field is ignored on finish).
 * ========================================================================= */

static trace_span_t g_noop_span = { .sampled = false };

/* ============================================================================
 * Clock helpers
 * ========================================================================= */

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Pseudo-random ID generation (fast, non-cryptographic)
 * ========================================================================= */

static _Atomic uint64_t g_id_seed = 0;

static uint64_t gen_id(void) {
    uint64_t v = atomic_fetch_add_explicit(&g_id_seed, 0x9e3779b97f4a7c15ULL,
                                            memory_order_relaxed);
    v ^= v >> 30; v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27; v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
    return v ? v : 1;
}

/* ============================================================================
 * Layer 1: Span buffer operations
 * ========================================================================= */

static bool span_buffer_push(span_buffer_t* buf, const trace_span_t* span) {
    /* Claim a slot */
    uint64_t idx = atomic_fetch_add_explicit(&buf->head, 1, memory_order_relaxed);

    /* Check if buffer is full */
    uint64_t tail = atomic_load_explicit(&buf->tail, memory_order_acquire);
    if (idx - tail >= (uint64_t)buf->capacity) {
        /* Can't use this slot — decrement head is complex in MPSC, just mark filled
         * with a sentinel.  Exporter skips non-sampled spans anyway. */
        return false;
    }

    span_slot_t* slot = &buf->slots[idx & buf->mask];

    /* Wait briefly for slot to clear (exporter is the only reader) */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SPAN_SLOT_EMPTY) {
        if (++spin > 512) return false;  /* Give up; don't block producer */
    }

    slot->span = *span;
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED, memory_order_release);
    return true;
}

/* ============================================================================
 * Layer 2: Sampling policy
 * ========================================================================= */

static bool sampling_should_sample(sampling_state_t* s) {
    bool bp = atomic_load_explicit(&s->in_backpressure, memory_order_relaxed);
    uint32_t sample_w = bp ? s->backpressure_sample : s->always_sample;
    uint32_t drop_w   = bp ? s->backpressure_drop   : s->always_drop;
    uint32_t total    = sample_w + drop_w;
    if (total == 0) return true;  /* 0+0 → always sample */

    uint64_t counter =
        atomic_fetch_add_explicit(&s->sample_counter, 1, memory_order_relaxed);
    return (counter % total) < sample_w;
}

/*
 * Update backpressure state — called ONLY from exporter thread.
 * No concurrent writes on the policy state fields touched here.
 */
static void sampling_state_update(sampling_state_t* s, span_buffer_t* buf,
                                   uint64_t total_drops, uint64_t cached_time) {
    uint64_t head = atomic_load_explicit(&buf->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&buf->tail, memory_order_acquire);
    uint64_t depth = head > tail ? head - tail : 0;
    uint32_t fill_pct = buf->capacity
                        ? (uint32_t)(depth * 100 / buf->capacity) : 0;

    /* Signal A: queue fill */
    bool cur_fill = atomic_load_explicit(&s->bp_fill_signal, memory_order_relaxed);
    if (!cur_fill && fill_pct >= BP_FILL_ENTER_PCT)
        atomic_store_explicit(&s->bp_fill_signal, true,  memory_order_relaxed);
    else if (cur_fill && fill_pct < BP_FILL_EXIT_PCT)
        atomic_store_explicit(&s->bp_fill_signal, false, memory_order_relaxed);

    /* Signal B: sustained drop rate (exporter-thread-only fields; no atomics) */
    if (s->drop_window_start_ns == 0) {
        s->drop_window_start_ns  = cached_time;
        s->drops_at_window_start = total_drops;
    }
    uint64_t elapsed = cached_time - s->drop_window_start_ns;
    if (elapsed >= DROP_WINDOW_NS) {
        uint64_t new_drops = total_drops - s->drops_at_window_start;
        bool cur_drop = atomic_load_explicit(&s->bp_drop_signal, memory_order_relaxed);
        if (!cur_drop && new_drops >= DROP_RATE_ENTER_THRESHOLD) {
            atomic_store_explicit(&s->bp_drop_signal, true, memory_order_relaxed);
            s->last_drop_seen_ns = cached_time;
        }
        /* Reset window */
        s->drop_window_start_ns  = cached_time;
        s->drops_at_window_start = total_drops;
    }

    /* Exit drop signal after clear window */
    if (atomic_load_explicit(&s->bp_drop_signal, memory_order_relaxed)) {
        if (total_drops == s->drops_at_window_start &&
            cached_time - s->last_drop_seen_ns >= DROP_CLEAR_WINDOW_NS) {
            atomic_store_explicit(&s->bp_drop_signal, false, memory_order_relaxed);
        }
    }

    bool combined =
        atomic_load_explicit(&s->bp_fill_signal, memory_order_relaxed) ||
        atomic_load_explicit(&s->bp_drop_signal, memory_order_relaxed);
    atomic_store_explicit(&s->in_backpressure, combined, memory_order_release);
}

/* ============================================================================
 * Layer 3: Exporter thread
 * ========================================================================= */

static void* exporter_thread_fn(void* arg) {
    tracer_t* t = (tracer_t*)arg;

    /* Seed ID generator with thread-dependent value */
    atomic_fetch_add_explicit(&g_id_seed, monotonic_ns(), memory_order_relaxed);

    while (!atomic_load_explicit(&t->shutdown, memory_order_acquire)) {
        /* Sleep for export interval (interruptible by shutdown) */
        struct timespec sleep_ts = {
            .tv_sec  = t->export_interval_ms / 1000,
            .tv_nsec = (t->export_interval_ms % 1000) * 1000000L
        };
        nanosleep(&sleep_ts, NULL);

        /* Refresh cached time */
        uint64_t now = monotonic_ns();
        atomic_store_explicit(&t->cached_time_ns, now, memory_order_relaxed);

        /* Update sampling policy */
        uint64_t drops =
            atomic_load_explicit(&t->spans_dropped_backpressure, memory_order_relaxed);
        sampling_state_update(&t->sampling, &t->buffer, drops, now);

        /* Drain filled slots */
        uint64_t tail = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&t->buffer.head, memory_order_acquire);

        /* Temporary export batch on stack — bounded size */
        trace_span_t export_batch[256];
        size_t batch_count = 0;

        while (tail < head && batch_count < 256) {
            span_slot_t* slot = &t->buffer.slots[tail & t->buffer.mask];
            uint32_t state =
                atomic_load_explicit(&slot->state, memory_order_acquire);
            if (state != SPAN_SLOT_FILLED) {
                if (state == SPAN_SLOT_EMPTY) {
                    /* Slot not yet filled by producer — skip for now */
                    break;
                }
                tail++;
                continue;
            }

            atomic_store_explicit(&slot->state, SPAN_SLOT_PROCESSING,
                                  memory_order_relaxed);

            /* Copy span data before releasing slot */
            if (slot->span.sampled)
                export_batch[batch_count++] = slot->span;

            atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY,
                                  memory_order_release);
            tail++;
        }
        atomic_store_explicit(&t->buffer.tail, tail, memory_order_release);

        /* Export batch */
        if (batch_count > 0 && t->export_callback) {
            atomic_fetch_add_explicit(&t->exports_attempted, 1, memory_order_relaxed);
            t->export_callback(export_batch, batch_count, t->user_data);
            atomic_fetch_add_explicit(&t->exports_succeeded, 1, memory_order_relaxed);
        }

        /* Update Prometheus gauges if registered */
        if (t->metrics_registered) {
            uint64_t h =
                atomic_load_explicit(&t->buffer.head, memory_order_relaxed);
            uint64_t tl =
                atomic_load_explicit(&t->buffer.tail, memory_order_relaxed);
            uint64_t depth = h > tl ? h - tl : 0;

            bool bp = atomic_load_explicit(&t->sampling.in_backpressure,
                                           memory_order_relaxed);
            uint32_t sample_w = bp ? t->sampling.backpressure_sample
                                   : t->sampling.always_sample;
            uint32_t drop_w   = bp ? t->sampling.backpressure_drop
                                   : t->sampling.always_drop;
            uint32_t total    = sample_w + drop_w;
            uint32_t pct      = total ? (sample_w * 100 / total) : 100;

            if (t->metrics_handles.queue_depth)
                metrics_gauge_set(t->metrics_handles.queue_depth, (double)depth);
            if (t->metrics_handles.sample_rate_pct)
                metrics_gauge_set(t->metrics_handles.sample_rate_pct, (double)pct);
            if (t->metrics_handles.spans_dropped)
                metrics_gauge_set(t->metrics_handles.spans_dropped,
                                  (double)atomic_load_explicit(
                                      &t->spans_dropped_backpressure,
                                      memory_order_relaxed));
            if (t->metrics_handles.spans_sampled_out)
                metrics_gauge_set(t->metrics_handles.spans_sampled_out,
                                  (double)atomic_load_explicit(
                                      &t->spans_sampled_out, memory_order_relaxed));
            if (t->metrics_handles.in_backpressure)
                metrics_gauge_set(t->metrics_handles.in_backpressure,
                                  bp ? 1.0 : 0.0);
        }
    }

    /* Final drain on shutdown */
    {
        uint64_t tail = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&t->buffer.head, memory_order_acquire);
        trace_span_t export_batch[256];
        size_t batch_count = 0;

        while (tail < head && batch_count < 256) {
            span_slot_t* slot = &t->buffer.slots[tail & t->buffer.mask];
            uint32_t state =
                atomic_load_explicit(&slot->state, memory_order_acquire);
            if (state == SPAN_SLOT_FILLED) {
                if (slot->span.sampled)
                    export_batch[batch_count++] = slot->span;
                atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY,
                                      memory_order_release);
            }
            tail++;
        }
        if (batch_count > 0 && t->export_callback)
            t->export_callback(export_batch, batch_count, t->user_data);
    }

    return NULL;
}

/* ============================================================================
 * Init / Destroy
 * ========================================================================= */

static distric_err_t tracer_alloc_and_init(tracer_t** out,
                                             const tracer_config_t* cfg) {
    tracer_t* t = calloc(1, sizeof(*t));
    if (!t) return DISTRIC_ERR_ALLOC_FAILURE;

    /* Layer 1: span buffer */
    size_t cap = cfg->buffer_capacity ? cfg->buffer_capacity
                                      : SPAN_BUFFER_DEFAULT_CAPACITY;
    /* Round up to power of 2; cap to hard limit */
    size_t p2 = 1;
    while (p2 < cap) p2 <<= 1;
    if (p2 > DISTRIC_MAX_SPANS_BUFFER) p2 = DISTRIC_MAX_SPANS_BUFFER;
    cap = p2;

    t->buffer.slots = calloc(cap, sizeof(span_slot_t));
    if (!t->buffer.slots) { free(t); return DISTRIC_ERR_ALLOC_FAILURE; }

    for (size_t i = 0; i < cap; i++)
        atomic_init(&t->buffer.slots[i].state, SPAN_SLOT_EMPTY);

    t->buffer.capacity = (uint32_t)cap;
    t->buffer.mask     = (uint32_t)(cap - 1);
    atomic_init(&t->buffer.head, 0);
    atomic_init(&t->buffer.tail, 0);

    /* Layer 2: sampling policy (immutable after this point) */
    t->sampling.always_sample       = cfg->sampling.always_sample;
    t->sampling.always_drop         = cfg->sampling.always_drop;
    t->sampling.backpressure_sample = cfg->sampling.backpressure_sample;
    t->sampling.backpressure_drop   = cfg->sampling.backpressure_drop;
    atomic_init(&t->sampling.in_backpressure, false);
    atomic_init(&t->sampling.bp_fill_signal,  false);
    atomic_init(&t->sampling.bp_drop_signal,  false);
    atomic_init(&t->sampling.sample_counter,  0);

    /* Layer 3: export scheduling */
    t->export_interval_ms = cfg->export_interval_ms
                            ? cfg->export_interval_ms
                            : SPAN_EXPORT_INTERVAL_MS_DEFAULT;
    t->export_callback = cfg->export_callback;
    t->user_data       = cfg->user_data;

    /* Counters */
    atomic_init(&t->spans_created,              0);
    atomic_init(&t->spans_sampled_in,           0);
    atomic_init(&t->spans_sampled_out,          0);
    atomic_init(&t->spans_dropped_backpressure, 0);
    atomic_init(&t->exports_attempted,          0);
    atomic_init(&t->exports_succeeded,          0);

    /* Lifecycle */
    atomic_init(&t->refcount, 1);
    atomic_init(&t->shutdown, false);
    t->metrics_registered = false;

    /* Seed time cache */
    atomic_init(&t->cached_time_ns, monotonic_ns());

    /* Start exporter thread */
    if (pthread_create(&t->exporter_thread, NULL, exporter_thread_fn, t) != 0) {
        free(t->buffer.slots);
        free(t);
        return DISTRIC_ERR_INIT_FAILED;
    }
    t->exporter_started = true;

    *out = t;
    return DISTRIC_OK;
}

distric_err_t trace_init(tracer_t** tracer,
                          void (*export_cb)(trace_span_t*, size_t, void*),
                          void* user_data) {
    tracer_config_t cfg = {
        .sampling = { .always_sample = 1, .always_drop = 0,
                      .backpressure_sample = 1, .backpressure_drop = 9 },
        .export_callback = export_cb,
        .user_data       = user_data,
    };
    return tracer_alloc_and_init(tracer, &cfg);
}

distric_err_t trace_init_with_sampling(tracer_t** tracer,
                                        const trace_sampling_config_t* sampling,
                                        void (*export_cb)(trace_span_t*, size_t, void*),
                                        void* user_data) {
    if (!sampling) return DISTRIC_ERR_INVALID_ARG;
    tracer_config_t cfg = {
        .sampling        = *sampling,
        .export_callback = export_cb,
        .user_data       = user_data,
    };
    return tracer_alloc_and_init(tracer, &cfg);
}

distric_err_t trace_init_with_config(tracer_t** tracer,
                                      const tracer_config_t* config) {
    if (!config || !config->export_callback) return DISTRIC_ERR_INVALID_ARG;
    return tracer_alloc_and_init(tracer, config);
}

void trace_retain(tracer_t* t) {
    if (!t) return;
    TRACER_ASSERT_LIFECYCLE(
        atomic_load_explicit(&t->refcount, memory_order_relaxed) > 0);
    atomic_fetch_add_explicit(&t->refcount, 1, memory_order_relaxed);
}

void trace_release(tracer_t* t) {
    if (!t) return;
    uint32_t prev = atomic_fetch_sub_explicit(&t->refcount, 1,
                                               memory_order_acq_rel);
    TRACER_ASSERT_LIFECYCLE(prev > 0);
    if (prev == 1) trace_destroy(t);
}

void trace_destroy(tracer_t* t) {
    if (!t) return;
    if (t->exporter_started) {
        atomic_store_explicit(&t->shutdown, true, memory_order_release);
        pthread_join(t->exporter_thread, NULL);
    }
    free(t->buffer.slots);
    free(t);
}

/* ============================================================================
 * Stats snapshot
 * ========================================================================= */

void trace_get_stats(tracer_t* t, tracer_stats_t* out) {
    if (!t || !out) return;
    memset(out, 0, sizeof(*out));

    out->spans_created =
        atomic_load_explicit(&t->spans_created, memory_order_relaxed);
    out->spans_sampled_in =
        atomic_load_explicit(&t->spans_sampled_in, memory_order_relaxed);
    out->spans_sampled_out =
        atomic_load_explicit(&t->spans_sampled_out, memory_order_relaxed);
    out->spans_dropped_backpressure =
        atomic_load_explicit(&t->spans_dropped_backpressure, memory_order_relaxed);
    out->exports_attempted =
        atomic_load_explicit(&t->exports_attempted, memory_order_relaxed);
    out->exports_succeeded =
        atomic_load_explicit(&t->exports_succeeded, memory_order_relaxed);

    uint64_t h = atomic_load_explicit(&t->buffer.head, memory_order_relaxed);
    uint64_t tl= atomic_load_explicit(&t->buffer.tail, memory_order_relaxed);
    out->queue_depth    = h > tl ? h - tl : 0;
    out->queue_capacity = t->buffer.capacity;

    out->in_backpressure =
        atomic_load_explicit(&t->sampling.in_backpressure, memory_order_relaxed);

    bool bp = out->in_backpressure;
    uint32_t sw = bp ? t->sampling.backpressure_sample : t->sampling.always_sample;
    uint32_t dw = bp ? t->sampling.backpressure_drop   : t->sampling.always_drop;
    uint32_t tot = sw + dw;
    out->effective_sample_rate_pct = tot ? (sw * 100 / tot) : 100;
}

/* ============================================================================
 * Metrics registration (Improvement #4)
 * ========================================================================= */

distric_err_t trace_register_metrics(tracer_t* t, metrics_registry_t* registry) {
    if (!t || !registry) return DISTRIC_ERR_INVALID_ARG;
    if (t->metrics_registered)  return DISTRIC_ERR_ALREADY_EXISTS;

    distric_err_t err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_queue_depth",
        "Current number of spans in the tracer queue",
        NULL, 0, &t->metrics_handles.queue_depth);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_sample_rate_pct",
        "Effective sampling rate percentage (0-100)",
        NULL, 0, &t->metrics_handles.sample_rate_pct);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_spans_dropped",
        "Cumulative spans dropped due to buffer overflow or backpressure",
        NULL, 0, &t->metrics_handles.spans_dropped);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_spans_sampled_out",
        "Cumulative spans excluded by the sampling policy",
        NULL, 0, &t->metrics_handles.spans_sampled_out);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_in_backpressure",
        "1 if the tracer is currently in backpressure mode, 0 otherwise",
        NULL, 0, &t->metrics_handles.in_backpressure);
    if (err != DISTRIC_OK) return err;

    t->metrics_registered = true;
    return DISTRIC_OK;
}

/* ============================================================================
 * Span operations
 * ========================================================================= */

static distric_err_t start_span_internal(tracer_t* t,
                                          trace_id_t trace_id,
                                          span_id_t  parent_id,
                                          const char* operation,
                                          trace_span_t** out) {
    if (!t || !operation || !out) return DISTRIC_ERR_INVALID_ARG;
    if (atomic_load_explicit(&t->shutdown, memory_order_acquire))
        return DISTRIC_ERR_SHUTDOWN;

    atomic_fetch_add_explicit(&t->spans_created, 1, memory_order_relaxed);

    /* Check buffer pressure */
    uint64_t h  = atomic_load_explicit(&t->buffer.head, memory_order_relaxed);
    uint64_t tl = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
    if (h - tl >= (uint64_t)t->buffer.capacity) {
        atomic_fetch_add_explicit(&t->spans_dropped_backpressure, 1,
                                  memory_order_relaxed);
        *out = &g_noop_span;
        return DISTRIC_ERR_BACKPRESSURE;
    }

    /* Sampling decision */
    if (!sampling_should_sample(&t->sampling)) {
        atomic_fetch_add_explicit(&t->spans_sampled_out, 1, memory_order_relaxed);
        *out = &g_noop_span;
        return DISTRIC_OK;  /* Not an error; span is a no-op */
    }
    atomic_fetch_add_explicit(&t->spans_sampled_in, 1, memory_order_relaxed);

    /* Allocate a span slot */
    uint64_t idx = atomic_fetch_add_explicit(&t->buffer.head, 1,
                                              memory_order_relaxed);
    span_slot_t* slot = &t->buffer.slots[idx & t->buffer.mask];

    /* Brief spin for slot to clear */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SPAN_SLOT_EMPTY) {
        if (++spin > 512) {
            /* Buffer under heavy load; back out */
            atomic_fetch_add_explicit(&t->spans_dropped_backpressure, 1,
                                      memory_order_relaxed);
            *out = &g_noop_span;
            return DISTRIC_ERR_BACKPRESSURE;
        }
    }

    trace_span_t* span = &slot->span;
    memset(span, 0, sizeof(*span));
    span->trace_id      = trace_id.high ? trace_id :
                          (trace_id_t){ .high = gen_id(), .low = gen_id() };
    span->span_id       = gen_id();
    span->parent_span_id = parent_id;
    span->sampled       = true;
    span->status        = SPAN_STATUS_UNSET;
    span->_tracer       = t;

    /* Use cached time (no syscall on hot path) */
    span->start_time_ns =
        atomic_load_explicit(&t->cached_time_ns, memory_order_relaxed);
    if (span->start_time_ns == 0)
        span->start_time_ns = realtime_ns();

    strncpy(span->operation, operation, DISTRIC_MAX_OPERATION_LEN - 1);

    /* Mark slot as in-progress (PROCESSING) so exporter doesn't drain it early */
    atomic_store_explicit(&slot->state, SPAN_SLOT_PROCESSING, memory_order_release);

    *out = span;
    return DISTRIC_OK;
}

distric_err_t trace_start_span(tracer_t* t, const char* op, trace_span_t** out) {
    trace_id_t zero = {0};
    return start_span_internal(t, zero, 0, op, out);
}

distric_err_t trace_start_child_span(tracer_t* t, trace_span_t* parent,
                                      const char* op, trace_span_t** out) {
    if (!parent) return trace_start_span(t, op, out);
    trace_id_t zero = { parent->sampled ? parent->trace_id.high : 0,
                         parent->sampled ? parent->trace_id.low  : 0 };
    span_id_t pid = parent->sampled ? parent->span_id : 0;
    return start_span_internal(t, zero, pid, op, out);
}

distric_err_t trace_start_span_from_context(tracer_t* t,
                                              const trace_context_t* ctx,
                                              const char* op,
                                              trace_span_t** out) {
    if (!ctx) return trace_start_span(t, op, out);
    trace_id_t tid = { ctx->trace_id.high, ctx->trace_id.low };
    return start_span_internal(t, tid, ctx->span_id, op, out);
}

distric_err_t trace_finish_span(tracer_t* t, trace_span_t* span) {
    if (!span) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;  /* no-op span */

    span->end_time_ns =
        atomic_load_explicit(&t->cached_time_ns, memory_order_relaxed);
    if (span->end_time_ns <= span->start_time_ns)
        span->end_time_ns = realtime_ns();

    /* Find the owning slot and mark as FILLED for exporter */
    tracer_t* owner = (tracer_t*)span->_tracer;
    if (!owner) return DISTRIC_ERR_INVALID_ARG;

    /* The span lives inside a slot; compute slot pointer */
    span_slot_t* slot = (span_slot_t*)((char*)span -
                         offsetof(span_slot_t, span));
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED, memory_order_release);

    return DISTRIC_OK;
}

distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value) {
    if (!span || !key) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;
    if (span->tag_count >= DISTRIC_MAX_SPAN_TAGS) return DISTRIC_ERR_REGISTRY_FULL;

    span_tag_t* tag = &span->tags[span->tag_count++];
    strncpy(tag->key,   key,   DISTRIC_MAX_TAG_KEY_LEN   - 1);
    strncpy(tag->value, value ? value : "", DISTRIC_MAX_TAG_VALUE_LEN - 1);
    return DISTRIC_OK;
}

distric_err_t trace_set_status(trace_span_t* span, span_status_t status) {
    if (!span) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;
    span->status = (int)status;
    return DISTRIC_OK;
}

distric_err_t trace_inject_context(trace_span_t* span, char* buf, size_t sz) {
    if (!span || !buf || sz == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) { buf[0] = '\0'; return DISTRIC_OK; }
    int n = snprintf(buf, sz, "%016llx%016llx-%016llx",
                     (unsigned long long)span->trace_id.high,
                     (unsigned long long)span->trace_id.low,
                     (unsigned long long)span->span_id);
    return (n > 0 && (size_t)n < sz) ? DISTRIC_OK : DISTRIC_ERR_BUFFER_OVERFLOW;
}

distric_err_t trace_extract_context(const char* header, trace_context_t* out) {
    if (!header || !out) return DISTRIC_ERR_INVALID_ARG;
    unsigned long long th, tl, sid;
    if (sscanf(header, "%16llx%16llx-%16llx", &th, &tl, &sid) != 3)
        return DISTRIC_ERR_INVALID_ARG;
    out->trace_id = (trace_id_t){ .high = th, .low = tl };
    out->span_id  = (span_id_t)sid;
    return DISTRIC_OK;
}