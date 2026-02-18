/*
 * tracing.c — DistriC Observability Library — Tracing Implementation
 *
 * =============================================================================
 * PRODUCTION HARDENING APPLIED
 * =============================================================================
 *
 * #1 Memory-Ordering Audit
 *   - head: fetch_add(relaxed) for claim; load(acquire) by exporter.
 *   - tail: store(release) by exporter; load(acquire) by producers.
 *   - slot->state: release on publish, acquire on consume, release on clear.
 *   - in_backpressure: store(release) by exporter; load(ACQUIRE) by producers.
 *     (Critical fix from relaxed — ensures producers see latest policy.)
 *   - last_export_ns, cached_time_ns: relaxed (approximate values only).
 *
 * #2 Ring Buffer Correctness
 *   - tail is _Atomic uint64_t; consumer writes with atomic store(release).
 *   - Producer slot-claim spin bounded by SPAN_SLOT_CLAIM_MAX_SPIN → drop.
 *
 * #3 Cache Line Separation (see tracing.h)
 *   - span_buffer_t.head and .tail on separate alignas(64) cache lines.
 *
 * #5 Exporter Thread Failure Detection
 *   - last_export_ns stamped (relaxed) at start of each exporter cycle.
 *   - trace_is_exporter_healthy() checks staleness vs 3x export interval.
 *
 * #8 Sampling Stability Under Backpressure
 *   - in_backpressure: producers load with acquire (was relaxed).
 *   - Hysteresis: separate enter/exit thresholds for fill and drop-rate signals.
 *
 * #9 Self-Monitoring via trace_register_internal_metrics()
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
#include <stdalign.h>
#include <stddef.h>  /* offsetof */

/* Fixed operation name length matches trace_span_t.operation[128] */
#define SPAN_OPERATION_LEN 128u

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
 * ID generation — xorshift64 seeded by monotonic clock + global counter
 * ========================================================================= */

static _Atomic uint64_t g_id_seed = 0;

static uint64_t gen_id(void) {
    uint64_t v = atomic_fetch_add_explicit(&g_id_seed, 1, memory_order_relaxed);
    v ^= v << 13;
    v ^= v >> 7;
    v ^= v << 17;
    return v | 1ULL;  /* never zero */
}

/* ============================================================================
 * Global no-op span — returned when a span is sampled-out or dropped.
 * Must never be written after return from trace_start_*.
 * ========================================================================= */
static trace_span_t g_noop_span = { .sampled = false };

/* ============================================================================
 * Sampling policy (Item #8)
 *
 * sampling_should_sample() — hot path, any producer thread.
 * sampling_state_update()  — called ONLY by the exporter thread (single writer).
 * ========================================================================= */

static bool sampling_should_sample(sampling_state_t* s) {
    /*
     * CRITICAL (Item #8): ACQUIRE load ensures producers see the exporter's
     * release-store.  Relaxed here would let producers miss BP-enter events
     * and continue at 100% rate while the buffer overflows.
     */
    bool bp = atomic_load_explicit(&s->in_backpressure, memory_order_acquire);
    uint32_t sw    = bp ? s->backpressure_sample : s->always_sample;
    uint32_t dw    = bp ? s->backpressure_drop   : s->always_drop;
    uint32_t total = sw + dw;
    if (total == 0) return false;
    if (dw == 0)    return true;
    uint64_t cnt = atomic_fetch_add_explicit(&s->sample_counter, 1,
                                             memory_order_relaxed);
    return (cnt % total) < sw;
}

static void sampling_state_update(sampling_state_t* s,
                                   const span_buffer_t* buf,
                                   uint64_t total_drops,
                                   uint64_t now_ns) {
    /* --- Signal A: queue fill hysteresis --- */
    uint64_t head  = atomic_load_explicit(&buf->head, memory_order_relaxed);
    uint64_t tail  = atomic_load_explicit(&buf->tail, memory_order_relaxed);
    uint64_t depth = (head > tail) ? (head - tail) : 0;
    bool fill_sig  = atomic_load_explicit(&s->bp_fill_signal, memory_order_relaxed);

    if (!fill_sig && depth * 100 >= (uint64_t)buf->capacity * BP_FILL_ENTER_PCT)
        fill_sig = true;
    else if (fill_sig && depth * 100 < (uint64_t)buf->capacity * BP_FILL_EXIT_PCT)
        fill_sig = false;
    atomic_store_explicit(&s->bp_fill_signal, fill_sig, memory_order_relaxed);

    /* --- Signal B: sustained drop rate hysteresis --- */
    bool drop_sig = atomic_load_explicit(&s->bp_drop_signal, memory_order_relaxed);
    if (s->drop_window_start_ns == 0) {
        s->drop_window_start_ns  = now_ns;
        s->drops_at_window_start = total_drops;
    }
    uint64_t window_drops = total_drops - s->drops_at_window_start;
    uint64_t window_age   = now_ns - s->drop_window_start_ns;

    if (!drop_sig) {
        if (window_drops >= DROP_RATE_ENTER_THRESHOLD) {
            drop_sig = true;
            s->last_drop_seen_ns = now_ns;
        }
        if (window_drops == 0 && window_age > DROP_WINDOW_NS) {
            s->drop_window_start_ns  = now_ns;
            s->drops_at_window_start = total_drops;
        }
    } else {
        if (window_drops > 0) {
            s->last_drop_seen_ns     = now_ns;
            s->drop_window_start_ns  = now_ns;
            s->drops_at_window_start = total_drops;
        }
        if (now_ns - s->last_drop_seen_ns >= DROP_CLEAR_WINDOW_NS)
            drop_sig = false;
    }
    atomic_store_explicit(&s->bp_drop_signal, drop_sig, memory_order_relaxed);

    /*
     * PUBLISH combined state with RELEASE.
     * Producers acquire-load this flag (see sampling_should_sample above).
     */
    bool combined = fill_sig || drop_sig;
    atomic_store_explicit(&s->in_backpressure, combined, memory_order_release);
}

/* ============================================================================
 * Exporter thread
 * ========================================================================= */

#define EXPORT_BATCH_MAX 64

static void* exporter_thread_fn(void* arg) {
    tracer_t* t = (tracer_t*)arg;

    /* Seed the ID generator with entropy */
    atomic_fetch_add_explicit(&g_id_seed, monotonic_ns(), memory_order_relaxed);

    trace_span_t* batch = malloc(EXPORT_BATCH_MAX * sizeof(trace_span_t));
    if (!batch) goto exit_cleanup;

    while (!atomic_load_explicit(&t->shutdown, memory_order_acquire)) {
        struct timespec sleep_ts = {
            .tv_sec  = (time_t)(t->export_interval_ms / 1000),
            .tv_nsec = (long)((t->export_interval_ms % 1000) * 1000000L)
        };
        nanosleep(&sleep_ts, NULL);

        uint64_t now = monotonic_ns();

        /*
         * Stamp liveness BEFORE doing work (Item #5).
         * Relaxed: health checker only tests approximate staleness.
         */
        atomic_store_explicit(&t->last_export_ns, now, memory_order_relaxed);
        atomic_store_explicit(&t->cached_time_ns, now, memory_order_relaxed);

        /* Update sampling policy (single-writer invariant) */
        uint64_t drops = atomic_load_explicit(&t->spans_dropped,
                                              memory_order_relaxed);
        sampling_state_update(&t->sampling, &t->buffer, drops, now);

        /* Drain filled slots (Items #1, #2) */
        uint64_t tail = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&t->buffer.head, memory_order_acquire);
        size_t   bc   = 0;

        while (tail < head && bc < EXPORT_BATCH_MAX) {
            span_slot_t* slot = &t->buffer.slots[tail & t->buffer.mask];
            uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);

            if (state == SPAN_SLOT_EMPTY) {
                /* Producer claimed slot but hasn't finished writing yet */
                break;
            }
            if (state == SPAN_SLOT_FILLED || state == SPAN_SLOT_PROCESSING) {
                if (slot->span.sampled)
                    batch[bc++] = slot->span;
                atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY,
                                      memory_order_release);
            }
            tail++;
        }

        /*
         * Advance tail with RELEASE (Item #1/#2).
         * Producers load tail with acquire for fullness checks.
         */
        atomic_store_explicit(&t->buffer.tail, tail, memory_order_release);

        if (bc > 0 && t->export_callback) {
            atomic_fetch_add_explicit(&t->exports_attempted, 1, memory_order_relaxed);
            t->export_callback(batch, bc, t->user_data);
            atomic_fetch_add_explicit(&t->exports_succeeded, 1, memory_order_relaxed);
        }

        /* Update Prometheus gauges (Item #9) */
        if (t->metrics_registered) {
            uint64_t h  = atomic_load_explicit(&t->buffer.head, memory_order_relaxed);
            uint64_t tl = atomic_load_explicit(&t->buffer.tail, memory_order_relaxed);
            uint64_t d  = (h > tl) ? (h - tl) : 0;
            bool bp     = atomic_load_explicit(&t->sampling.in_backpressure,
                                               memory_order_relaxed);
            uint32_t sw    = bp ? t->sampling.backpressure_sample : t->sampling.always_sample;
            uint32_t dw    = bp ? t->sampling.backpressure_drop   : t->sampling.always_drop;
            uint32_t total = sw + dw;
            uint32_t pct   = total ? (sw * 100u / total) : 0u;

            if (t->metrics_handles.queue_depth)
                metrics_gauge_set(t->metrics_handles.queue_depth, (double)d);
            if (t->metrics_handles.sample_rate_pct)
                metrics_gauge_set(t->metrics_handles.sample_rate_pct, (double)pct);
            if (t->metrics_handles.in_backpressure)
                metrics_gauge_set(t->metrics_handles.in_backpressure, bp ? 1.0 : 0.0);
            if (t->metrics_handles.spans_dropped)
                metrics_gauge_set(t->metrics_handles.spans_dropped,
                    (double)atomic_load_explicit(&t->spans_dropped,
                                                 memory_order_relaxed));
            if (t->metrics_handles.spans_sampled_out)
                metrics_gauge_set(t->metrics_handles.spans_sampled_out,
                    (double)atomic_load_explicit(&t->spans_sampled_out,
                                                 memory_order_relaxed));
            if (t->metrics_handles.exporter_alive)
                metrics_gauge_set(t->metrics_handles.exporter_alive, 1.0);
            if (t->metrics_handles.exports_succeeded)
                metrics_gauge_set(t->metrics_handles.exports_succeeded,
                    (double)atomic_load_explicit(&t->exports_succeeded,
                                                 memory_order_relaxed));
        }
    }

    /* Final drain on graceful shutdown */
    {
        uint64_t tail = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&t->buffer.head, memory_order_acquire);
        size_t   bc   = 0;
        while (tail < head && bc < EXPORT_BATCH_MAX) {
            span_slot_t* slot = &t->buffer.slots[tail & t->buffer.mask];
            uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
            if ((state == SPAN_SLOT_FILLED || state == SPAN_SLOT_PROCESSING)
                && slot->span.sampled) {
                batch[bc++] = slot->span;
            }
            atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY, memory_order_release);
            tail++;
        }
        atomic_store_explicit(&t->buffer.tail, tail, memory_order_release);
        if (bc > 0 && t->export_callback)
            t->export_callback(batch, bc, t->user_data);
    }

    free(batch);

exit_cleanup:
    if (t->metrics_registered && t->metrics_handles.exporter_alive)
        metrics_gauge_set(t->metrics_handles.exporter_alive, 0.0);
    return NULL;
}

#undef EXPORT_BATCH_MAX

/* ============================================================================
 * Init / Destroy / Lifecycle
 * ========================================================================= */

static distric_err_t tracer_alloc_and_init(tracer_t** out,
                                             const tracer_config_t* cfg) {
    tracer_t* t = calloc(1, sizeof(*t));
    if (!t) return DISTRIC_ERR_ALLOC_FAILURE;

    /* Round buffer capacity up to power of 2 */
    size_t cap = cfg->buffer_capacity ? cfg->buffer_capacity
                                      : SPAN_BUFFER_DEFAULT_CAPACITY;
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

    /* Sampling policy — immutable after init */
    t->sampling.always_sample       = cfg->sampling.always_sample;
    t->sampling.always_drop         = cfg->sampling.always_drop;
    t->sampling.backpressure_sample = cfg->sampling.backpressure_sample;
    t->sampling.backpressure_drop   = cfg->sampling.backpressure_drop;
    atomic_init(&t->sampling.in_backpressure, false);
    atomic_init(&t->sampling.bp_fill_signal,  false);
    atomic_init(&t->sampling.bp_drop_signal,  false);
    atomic_init(&t->sampling.sample_counter,  0);

    t->export_interval_ms = cfg->export_interval_ms
                            ? cfg->export_interval_ms
                            : SPAN_EXPORT_INTERVAL_MS_DEFAULT;
    t->export_callback = cfg->export_callback;
    t->user_data       = cfg->user_data;

    atomic_init(&t->spans_created,     0);
    atomic_init(&t->spans_sampled_in,  0);
    atomic_init(&t->spans_sampled_out, 0);
    atomic_init(&t->spans_dropped,     0);
    atomic_init(&t->exports_attempted, 0);
    atomic_init(&t->exports_succeeded, 0);

    atomic_init(&t->refcount,       1);
    atomic_init(&t->shutdown,       false);
    atomic_init(&t->cached_time_ns, monotonic_ns());
    atomic_init(&t->last_export_ns, 0);
    t->metrics_registered = false;
    t->exporter_started   = false;

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
    if (!tracer || !export_cb) return DISTRIC_ERR_INVALID_ARG;
    tracer_config_t cfg = {
        .sampling = {
            .always_sample       = 1, .always_drop         = 0,
            .backpressure_sample = 1, .backpressure_drop   = 9,
        },
        .export_callback = export_cb,
        .user_data       = user_data,
    };
    return tracer_alloc_and_init(tracer, &cfg);
}

distric_err_t trace_init_with_sampling(
    tracer_t**                     tracer,
    const trace_sampling_config_t* sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void*                          user_data) {
    if (!tracer || !sampling || !export_callback) return DISTRIC_ERR_INVALID_ARG;
    tracer_config_t cfg = {
        .sampling        = *sampling,
        .export_callback = export_callback,
        .user_data       = user_data,
    };
    return tracer_alloc_and_init(tracer, &cfg);
}

distric_err_t trace_init_with_config(tracer_t** tracer,
                                      const tracer_config_t* config) {
    if (!tracer || !config || !config->export_callback)
        return DISTRIC_ERR_INVALID_ARG;
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
 * trace_id_t helpers — keep 128-bit ID manipulation local to this TU
 * ========================================================================= */

/** Return a fresh 128-bit trace ID by generating two independent 64-bit words. */
static trace_id_t gen_trace_id(void) {
    trace_id_t id;
    id.high = gen_id();
    id.low  = gen_id();
    return id;
}

/** True when both halves of the 128-bit ID are zero (the "unset" sentinel). */
static bool trace_id_is_zero(trace_id_t id) {
    return id.high == 0 && id.low == 0;
}

/** Construct the zero/unset trace ID. */
static trace_id_t trace_id_zero(void) {
    trace_id_t id = { .high = 0, .low = 0 };
    return id;
}

/* ============================================================================
 * Internal span allocation helper
 * ========================================================================= */

static distric_err_t start_span_internal(tracer_t*   t,
                                          trace_id_t  trace_id,
                                          span_id_t   parent_id,
                                          const char* operation,
                                          trace_span_t** out) {
    if (!t || !operation || !out) return DISTRIC_ERR_INVALID_ARG;
    if (atomic_load_explicit(&t->shutdown, memory_order_acquire))
        return DISTRIC_ERR_SHUTDOWN;

    atomic_fetch_add_explicit(&t->spans_created, 1, memory_order_relaxed);

    /* Approximate fullness pre-check (relaxed for speed; definitive check after claim) */
    uint64_t h  = atomic_load_explicit(&t->buffer.head, memory_order_relaxed);
    uint64_t tl = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
    if (h - tl >= (uint64_t)t->buffer.capacity) {
        atomic_fetch_add_explicit(&t->spans_dropped, 1, memory_order_relaxed);
        *out = &g_noop_span;
        return DISTRIC_ERR_BACKPRESSURE;
    }

    /* Sampling decision (acquire on in_backpressure — Item #8) */
    if (!sampling_should_sample(&t->sampling)) {
        atomic_fetch_add_explicit(&t->spans_sampled_out, 1, memory_order_relaxed);
        *out = &g_noop_span;
        return DISTRIC_OK;
    }
    atomic_fetch_add_explicit(&t->spans_sampled_in, 1, memory_order_relaxed);

    /* Claim a slot — relaxed: slot state release-store is the synchronisation point */
    uint64_t idx = atomic_fetch_add_explicit(&t->buffer.head, 1,
                                              memory_order_relaxed);

    /* Definitive capacity re-check after claim (handles race with other producers) */
    tl = atomic_load_explicit(&t->buffer.tail, memory_order_acquire);
    if (idx - tl >= (uint64_t)t->buffer.capacity) {
        span_slot_t* lost = &t->buffer.slots[idx & t->buffer.mask];
        atomic_store_explicit(&lost->state, SPAN_SLOT_EMPTY, memory_order_release);
        atomic_fetch_add_explicit(&t->spans_dropped, 1, memory_order_relaxed);
        *out = &g_noop_span;
        return DISTRIC_ERR_BACKPRESSURE;
    }

    span_slot_t* slot = &t->buffer.slots[idx & t->buffer.mask];

    /*
     * Bounded spin for slot to become EMPTY (Item #2).
     * Drop rather than spin indefinitely — preserves non-blocking guarantee.
     * acquire: synchronise with exporter's release-store to EMPTY.
     */
    uint32_t spin = 0;
    while (atomic_load_explicit(&slot->state, memory_order_acquire) != SPAN_SLOT_EMPTY) {
        if (++spin > SPAN_SLOT_CLAIM_MAX_SPIN) {
            atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY, memory_order_release);
            atomic_fetch_add_explicit(&t->spans_dropped, 1, memory_order_relaxed);
            *out = &g_noop_span;
            return DISTRIC_ERR_BACKPRESSURE;
        }
        if (spin > 16) sched_yield();
    }

    TRACER_ASSERT_SLOT_STATE(slot, SPAN_SLOT_EMPTY);

    /* Initialise span fields */
    trace_span_t* span = &slot->span;
    memset(span, 0, sizeof(*span));

    span->trace_id       = trace_id_is_zero(trace_id) ? gen_trace_id() : trace_id;
    span->span_id        = gen_id();
    span->parent_span_id = parent_id;
    span->sampled        = true;
    span->status         = SPAN_STATUS_UNSET;

    /* Use cached time (avoids clock_gettime syscall on hot path) */
    span->start_time_ns = atomic_load_explicit(&t->cached_time_ns,
                                                memory_order_relaxed);
    if (span->start_time_ns == 0)
        span->start_time_ns = realtime_ns();

    strncpy(span->operation, operation, SPAN_OPERATION_LEN - 1);
    span->operation[SPAN_OPERATION_LEN - 1] = '\0';

    /*
     * Mark PROCESSING (release): exporter won't drain this slot until
     * trace_finish_span marks it FILLED.
     */
    atomic_store_explicit(&slot->state, SPAN_SLOT_PROCESSING, memory_order_release);

    *out = span;
    return DISTRIC_OK;
}

/* ============================================================================
 * Public span API — signatures match distric_obs.h exactly
 * ========================================================================= */

distric_err_t trace_start_span(tracer_t* t, const char* operation,
                                trace_span_t** out_span) {
    return start_span_internal(t, trace_id_zero(), 0, operation, out_span);
}

distric_err_t trace_start_child_span(tracer_t* t, trace_span_t* parent,
                                      const char* operation,
                                      trace_span_t** out_span) {
    trace_id_t tid = (parent && parent->sampled) ? parent->trace_id : trace_id_zero();
    span_id_t  pid = (parent && parent->sampled) ? parent->span_id  : 0;
    return start_span_internal(t, tid, pid, operation, out_span);
}

distric_err_t trace_start_span_from_context(tracer_t* t,
                                              const trace_context_t* ctx,
                                              const char* operation,
                                              trace_span_t** out_span) {
    trace_id_t tid = ctx ? ctx->trace_id : trace_id_zero();
    span_id_t  pid = ctx ? ctx->span_id  : 0;
    return start_span_internal(t, tid, pid, operation, out_span);
}

distric_err_t trace_finish_span(tracer_t* t, trace_span_t* span) {
    (void)t;
    if (!span || !span->sampled) return DISTRIC_OK;

    /* Stamp end time — fall back to realtime if cached time not yet available */
    uint64_t now = realtime_ns();
    if (now <= span->start_time_ns)
        now = span->start_time_ns + 1;
    span->end_time_ns = now;

    /*
     * Find the owning slot via pointer arithmetic (safe: sampled spans
     * always live inside a span_slot_t allocated from the ring buffer).
     * PUBLISH with release to synchronise span->end_time_ns write with
     * the exporter's acquire-load.
     */
    span_slot_t* slot = (span_slot_t*)((char*)span - offsetof(span_slot_t, span));
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED, memory_order_release);
    return DISTRIC_OK;
}

distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value) {
    if (!span || !key) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) return DISTRIC_OK;
    if (span->tag_count >= DISTRIC_MAX_SPAN_TAGS) return DISTRIC_ERR_BUFFER_OVERFLOW;
    span_tag_t* tag = &span->tags[span->tag_count++];
    strncpy(tag->key,   key,              DISTRIC_MAX_TAG_KEY_LEN   - 1);
    strncpy(tag->value, value ? value : "", DISTRIC_MAX_TAG_VALUE_LEN - 1);
    return DISTRIC_OK;
}

distric_err_t trace_set_status(trace_span_t* span, span_status_t status) {
    if (!span) return DISTRIC_ERR_INVALID_ARG;
    if (span->sampled) span->status = status;
    return DISTRIC_OK;
}

/* ============================================================================
 * Context propagation
 * ========================================================================= */

distric_err_t trace_inject_context(trace_span_t* span,
                                    char* header_buf, size_t buf_size) {
    if (!span || !header_buf || buf_size == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!span->sampled) { header_buf[0] = '\0'; return DISTRIC_OK; }
    /*
     * Format: <trace_id_high_hex><trace_id_low_hex>-<span_id_hex>
     * trace_id is 128-bit (two 64-bit words); span_id is 64-bit.
     * Total: 32 + 1 + 16 = 49 characters + NUL.
     */
    int n = snprintf(header_buf, buf_size,
                     "%016llx%016llx-%016llx",
                     (unsigned long long)span->trace_id.high,
                     (unsigned long long)span->trace_id.low,
                     (unsigned long long)span->span_id);
    return (n > 0 && (size_t)n < buf_size) ? DISTRIC_OK : DISTRIC_ERR_BUFFER_OVERFLOW;
}

distric_err_t trace_extract_context(const char* header, trace_context_t* out_ctx) {
    if (!header || !out_ctx) return DISTRIC_ERR_INVALID_ARG;
    unsigned long long tid_hi, tid_lo, sid;
    /*
     * Parse the format produced by trace_inject_context:
     *   <16-hex-trace-high><16-hex-trace-low>-<16-hex-span-id>
     */
    if (sscanf(header, "%16llx%16llx-%16llx", &tid_hi, &tid_lo, &sid) != 3)
        return DISTRIC_ERR_INVALID_FORMAT;
    out_ctx->trace_id.high = (uint64_t)tid_hi;
    out_ctx->trace_id.low  = (uint64_t)tid_lo;
    out_ctx->span_id       = (uint64_t)sid;
    return DISTRIC_OK;
}

/* ============================================================================
 * Thread-local active span
 * ========================================================================= */

static _Thread_local trace_span_t* tl_active_span = NULL;

void trace_set_active_span(trace_span_t* span) { tl_active_span = span; }
trace_span_t* trace_get_active_span(void)       { return tl_active_span; }

/* ============================================================================
 * Stats snapshot — all reads approximate (relaxed loads)
 * ========================================================================= */

void trace_get_stats(tracer_t* t, tracer_stats_t* out) {
    if (!t || !out) return;

    uint64_t h  = atomic_load_explicit(&t->buffer.head, memory_order_relaxed);
    uint64_t tl = atomic_load_explicit(&t->buffer.tail, memory_order_relaxed);
    bool     bp = atomic_load_explicit(&t->sampling.in_backpressure,
                                        memory_order_relaxed);
    uint32_t sw    = bp ? t->sampling.backpressure_sample : t->sampling.always_sample;
    uint32_t dw    = bp ? t->sampling.backpressure_drop   : t->sampling.always_drop;
    uint32_t total = sw + dw;

    out->spans_created              = atomic_load_explicit(&t->spans_created,     memory_order_relaxed);
    out->spans_sampled_in           = atomic_load_explicit(&t->spans_sampled_in,  memory_order_relaxed);
    out->spans_sampled_out          = atomic_load_explicit(&t->spans_sampled_out, memory_order_relaxed);
    out->spans_dropped_backpressure = atomic_load_explicit(&t->spans_dropped,     memory_order_relaxed);
    out->exports_attempted          = atomic_load_explicit(&t->exports_attempted, memory_order_relaxed);
    out->exports_succeeded          = atomic_load_explicit(&t->exports_succeeded, memory_order_relaxed);
    out->queue_depth                = (h > tl) ? (h - tl) : 0;
    out->queue_capacity             = t->buffer.capacity;
    out->in_backpressure            = bp;
    out->effective_sample_rate_pct  = total ? (sw * 100u / total) : 0u;
}

/* ============================================================================
 * Exporter health check (Item #5)
 * ========================================================================= */

bool trace_is_exporter_healthy(const tracer_t* t) {
    if (!t || !t->exporter_started) return false;
    uint64_t last = atomic_load_explicit(
        &((tracer_t*)t)->last_export_ns, memory_order_relaxed);
    if (last == 0) return true;  /* grace period — first export not yet done */
    uint64_t stale = (uint64_t)t->export_interval_ms * 1000000ULL
                     * TRACER_EXPORTER_STALE_MULTIPLIER;
    return (monotonic_ns() - last) < stale;
}

/* ============================================================================
 * Internal Prometheus metrics registration (Items #5, #8, #9)
 * ========================================================================= */

distric_err_t trace_register_metrics(tracer_t* t,
                                      metrics_registry_t* registry) {
    if (!t || !registry) return DISTRIC_ERR_INVALID_ARG;
    if (t->metrics_registered) return DISTRIC_ERR_ALREADY_EXISTS;

    distric_err_t err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_queue_depth",
        "Number of spans currently buffered in the tracer ring",
        NULL, 0, &t->metrics_handles.queue_depth);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_sample_rate_pct",
        "Effective span sample rate percentage (0-100)",
        NULL, 0, &t->metrics_handles.sample_rate_pct);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_drops_total",
        "Cumulative spans dropped due to ring backpressure",
        NULL, 0, &t->metrics_handles.spans_dropped);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_sampled_out_total",
        "Cumulative spans excluded by sampling policy",
        NULL, 0, &t->metrics_handles.spans_sampled_out);
    if (err != DISTRIC_OK) return err;

    err = metrics_register_gauge(registry,
        "distric_internal_tracer_backpressure_active",
        "1 if the tracer is currently in backpressure mode, 0 otherwise",
        NULL, 0, &t->metrics_handles.in_backpressure);
    if (err != DISTRIC_OK) return err;

    /* Item #5: exporter liveness */
    metrics_register_gauge(registry,
        "distric_internal_tracer_exporter_alive",
        "1 if the tracer exporter thread is alive and making progress, 0 otherwise",
        NULL, 0, &t->metrics_handles.exporter_alive);

    /* Item #9: export success counter */
    metrics_register_gauge(registry,
        "distric_internal_tracer_exports_succeeded",
        "Cumulative successful export callback invocations",
        NULL, 0, &t->metrics_handles.exports_succeeded);

    t->metrics_registered = true;

    if (t->exporter_started && t->metrics_handles.exporter_alive)
        metrics_gauge_set(t->metrics_handles.exporter_alive, 1.0);

    return DISTRIC_OK;
}