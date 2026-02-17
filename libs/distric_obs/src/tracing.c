/*
 * tracing.c — DistriC Observability Library — Distributed Tracing
 *
 * =============================================================================
 * ADAPTIVE SAMPLING — SUSTAINED BACKPRESSURE (Production Blocker #2)
 * =============================================================================
 *
 * The previous implementation only used queue fill percentage to decide whether
 * to enter or exit backpressure mode.  This is insufficient under sustained
 * exporter slowness: the queue can be below the fill threshold yet still
 * accumulate drops if the exporter is consistently slow.
 *
 * The updated backpressure model uses TWO independent signals:
 *
 *   Signal A — Queue fill:
 *     Enter at >75% fill.  Exit when fill drops below 50%.
 *     Hysteresis prevents oscillation during transient bursts.
 *
 *   Signal B — Sustained drop rate:
 *     Tracked by sampling spans_dropped_backpressure over a rolling window.
 *     If the number of new drops in the last DROP_WINDOW_NS nanoseconds
 *     exceeds DROP_RATE_ENTER_THRESHOLD, enter backpressure.
 *     Exit only when no drops occur for DROP_CLEAR_WINDOW_NS.
 *     This signal catches a stalled / slow exporter that is not yet causing
 *     a full queue but is already causing drops at a high rate.
 *
 *   Combined:  in_backpressure = (signal_A || signal_B)
 *              with separate hysteresis for each signal.
 *
 * Sampling rates:
 *   Normal mode:      always_sample / (always_sample + always_drop)
 *   Backpressure mode: backpressure_sample / (backpressure_sample +
 *                                              backpressure_drop)
 *   Default:          100% normal, 10% under backpressure.
 *
 * Stats exposure:
 *   Call trace_get_stats() to retrieve a snapshot of counters suitable for
 *   display, alerting, or re-export.  Call trace_register_metrics() to expose
 *   these stats as live Prometheus gauges in a metrics registry — this allows
 *   operators to observe tracing degradation in the same dashboard as other
 *   system metrics.
 *
 * Thread-safety model:
 *   trace_start_span / trace_finish_span: lock-free.
 *   trace_add_tag / trace_set_status: lock-free (caller-owned span).
 *   export loop: background thread only.
 *   trace_get_stats: safe from any thread (relaxed atomic reads).
 *   trace_register_metrics: must be called from a single thread before
 *     the tracer receives concurrent calls.
 *   trace_destroy: must be the last call on a tracer.
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
 *
 * DROP_WINDOW_NS:
 *   Length of the observation window for the drop-rate signal (1 second).
 *   If more than DROP_RATE_ENTER_THRESHOLD drops occur within this window,
 *   backpressure is entered.
 *
 * DROP_RATE_ENTER_THRESHOLD:
 *   Minimum new drops in one window that triggers backpressure.
 *
 * DROP_CLEAR_WINDOW_NS:
 *   A clean window of this length (no new drops) is required before the
 *   drop-rate signal is cleared.  Longer than the enter window to add
 *   hysteresis and prevent rapid toggling.
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

/*
 * Tracing-internal metrics handles.
 *
 * Populated by trace_register_metrics().  All fields are NULL until that
 * function is called; the export-loop writes to them only when non-NULL so
 * there is no risk of a NULL dereference on the hot path.
 */
typedef struct {
    metric_t* queue_depth;        /* gauge: current buffer occupancy    */
    metric_t* sample_rate_pct;    /* gauge: effective sampling rate [%] */
    metric_t* spans_dropped;      /* counter: cumulative drop count     */
    metric_t* spans_sampled_out;  /* counter: spans dropped by sampler  */
    metric_t* in_backpressure;    /* gauge: 0 or 1                      */
} tracer_internal_metrics_t;

struct tracer_s {
    span_slot_t* slots;
    uint32_t     buffer_mask;

    _Atomic uint64_t head;
    _Atomic uint64_t tail;

    trace_sampling_config_t sampling;

    /* ---- Backpressure state ---- */
    _Atomic bool     bp_fill_signal;       /* fill-% signal is active          */
    _Atomic bool     bp_drop_signal;       /* drop-rate signal is active        */
    _Atomic bool     in_backpressure;      /* combined (fill OR drop)           */

    /* Drop-rate window tracking (written only from exporter thread). */
    uint64_t         drop_window_start_ns;
    uint64_t         drops_at_window_start;
    uint64_t         last_drop_seen_ns;    /* wall-clock of most recent drop    */

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
 * Uses a global atomic seed; relaxed ordering is intentional — the seed
 * does not need to be precisely sequenced across threads.
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

/*
 * should_sample:
 *
 * Reads in_backpressure with relaxed ordering — this is an advisory hint.
 * A brief race where we use the wrong sampling rate for one decision is
 * acceptable; correctness of the system does not depend on atomicity here.
 */
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
 * Backpressure update (called from exporter thread only)
 *
 * Evaluates both signals and updates in_backpressure.
 *
 * Signal A — queue fill (hysteresis 75% enter / 50% exit):
 *   Protects against buffer exhaustion under burst load.
 *
 * Signal B — sustained drop rate:
 *   If more than DROP_RATE_ENTER_THRESHOLD spans are dropped in
 *   DROP_WINDOW_NS, backpressure is entered regardless of fill level.
 *   Clears only after DROP_CLEAR_WINDOW_NS of zero drops.
 *   This catches a slow exporter that is not yet causing a full queue.
 *
 * Calling convention: called after each export iteration, including idle
 * iterations.  All reads/writes to drop_window_* fields are from the
 * exporter thread only (no concurrent access, no atomics needed).
 * ========================================================================= */
static void update_backpressure(tracer_t* tracer) {
    const uint64_t now = get_time_ns();

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
        new_fill = (fill_pct > 50u); /* exit hysteresis */
    } else {
        new_fill = (fill_pct > 75u); /* entry threshold  */
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

    /* Advance drop-rate window. */
    if (now - tracer->drop_window_start_ns >= DROP_WINDOW_NS) {
        uint64_t new_drops = total_drops - tracer->drops_at_window_start;
        tracer->drops_at_window_start = total_drops;
        tracer->drop_window_start_ns  = now;

        if (new_drops >= DROP_RATE_ENTER_THRESHOLD) {
            /* New drops seen — enter (or stay in) drop backpressure. */
            tracer->last_drop_seen_ns = now;
            if (!cur_drop) {
                atomic_store_explicit(&tracer->bp_drop_signal, true,
                                      memory_order_relaxed);
                cur_drop = true;
            }
        }
    }

    /* Exit drop-signal after DROP_CLEAR_WINDOW_NS of silence. */
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
    if (m->queue_depth) {
        metrics_gauge_set(m->queue_depth, (double)used);
    }
    if (m->in_backpressure) {
        metrics_gauge_set(m->in_backpressure, new_bp ? 1.0 : 0.0);
    }
    if (m->sample_rate_pct) {
        bool bp2 = new_bp;
        uint32_t sn = bp2 ? tracer->sampling.backpressure_sample
                          : tracer->sampling.always_sample;
        uint32_t tn = bp2 ? (tracer->sampling.backpressure_sample +
                              tracer->sampling.backpressure_drop)
                          : (tracer->sampling.always_sample +
                              tracer->sampling.always_drop);
        double rate = (tn > 0) ? ((double)sn * 100.0 / (double)tn) : 0.0;
        metrics_gauge_set(m->sample_rate_pct, rate);
    }
}

/* ============================================================================
 * Exporter thread
 * ========================================================================= */

static void* exporter_thread_fn(void* arg) {
    tracer_t* tracer = (tracer_t*)arg;

    while (!atomic_load_explicit(&tracer->shutdown, memory_order_acquire)) {
        uint64_t tail = atomic_load_explicit(&tracer->tail,
                                              memory_order_acquire);
        uint64_t head = atomic_load_explicit(&tracer->head,
                                              memory_order_acquire);

        if (tail == head) {
            /* Buffer empty: sleep briefly then update backpressure signals. */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&ts, NULL);
            update_backpressure(tracer);
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

            /* Update cumulative drop / sampled-out metrics. */
            tracer_internal_metrics_t* mi = &tracer->metrics_handles;
            if (mi->spans_dropped) {
                uint64_t drops = atomic_load_explicit(
                    &tracer->spans_dropped_backpressure,
                    memory_order_relaxed);
                /* The counter stores total drops; gauge reflects current. */
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

        update_backpressure(tracer);
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
    /*
     * Default sampling: 100% normal, 10% under backpressure.
     * Tracing volume automatically drops by 90% when the export buffer
     * exceeds 75% fill or when sustained drops are detected.
     */
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

    atomic_init(&t->head,                        0);
    atomic_init(&t->tail,                        0);
    atomic_init(&t->spans_created,               0);
    atomic_init(&t->spans_sampled_in,            0);
    atomic_init(&t->spans_sampled_out,           0);
    atomic_init(&t->spans_dropped_backpressure,  0);
    atomic_init(&t->exports_attempted,           0);
    atomic_init(&t->exports_succeeded,           0);
    atomic_init(&t->sample_counter,              0);
    atomic_init(&t->bp_fill_signal,              false);
    atomic_init(&t->bp_drop_signal,              false);
    atomic_init(&t->in_backpressure,             false);
    atomic_init(&t->refcount,                    1);
    atomic_init(&t->shutdown,                    false);

    /* Initialise drop-rate window bookkeeping. */
    t->drop_window_start_ns  = get_time_ns();
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

void trace_destroy(tracer_t* tracer) {
    trace_release(tracer);
}

/* ============================================================================
 * Public API — statistics (Production Blocker #2: expose stats as metrics)
 * ========================================================================= */

/*
 * trace_get_stats:
 *
 * Non-blocking snapshot of the tracer's internal counters.  All reads are
 * relaxed (approximate); callers must not rely on precise ordering between
 * fields.  Suitable for dashboards, alerting, and logging.
 */
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
        &tracer->spans_dropped_backpressure, memory_order_relaxed);
    out->exports_attempted          = atomic_load_explicit(&tracer->exports_attempted,
                                                            memory_order_relaxed);
    out->exports_succeeded          = atomic_load_explicit(&tracer->exports_succeeded,
                                                            memory_order_relaxed);
    out->queue_depth                = used;
    out->queue_capacity             = tracer->buffer_mask + 1;
    out->in_backpressure            = atomic_load_explicit(&tracer->in_backpressure,
                                                            memory_order_relaxed);

    /* Effective sampling rate (%) given current backpressure state. */
    bool bp = out->in_backpressure;
    uint32_t sn = bp ? tracer->sampling.backpressure_sample
                     : tracer->sampling.always_sample;
    uint32_t tn = bp ? (tracer->sampling.backpressure_sample +
                         tracer->sampling.backpressure_drop)
                     : (tracer->sampling.always_sample +
                         tracer->sampling.always_drop);
    out->effective_sample_rate_pct = (tn > 0) ? (sn * 100u / tn) : 0u;
}

/*
 * trace_register_metrics:
 *
 * Registers five internal gauges in the provided metrics registry and wires
 * them to the tracer.  After this call the exporter loop updates these gauges
 * automatically on every export iteration so operators can observe tracing
 * degradation in Prometheus without polling trace_get_stats() manually.
 *
 * Metrics registered:
 *   distric_tracing_queue_depth      — current span buffer occupancy
 *   distric_tracing_sample_rate_pct  — effective sampling rate [0-100]
 *   distric_tracing_drops_total      — cumulative queue-overflow drops
 *   distric_tracing_sampled_out_total — cumulative sampler-dropped spans
 *   distric_tracing_in_backpressure  — 0 or 1
 *
 * Must be called once, from a single thread, before concurrent use.
 * Returns DISTRIC_OK even if some metrics fail to register (already-existing
 * names); partial registration is handled gracefully.
 */
distric_err_t trace_register_metrics(tracer_t*           tracer,
                                      metrics_registry_t* registry) {
    if (!tracer || !registry) return DISTRIC_ERR_INVALID_ARG;

    tracer_internal_metrics_t* m = &tracer->metrics_handles;

    /* Ignore individual registration errors — if a name is already taken we
     * simply do not get a handle and the field stays NULL.  The export loop
     * checks for NULL before writing. */
    metrics_register_gauge(registry,
        "distric_tracing_queue_depth",
        "Current number of spans queued for export",
        NULL, 0, &m->queue_depth);

    metrics_register_gauge(registry,
        "distric_tracing_sample_rate_pct",
        "Effective sampling rate as a percentage (0-100)",
        NULL, 0, &m->sample_rate_pct);

    metrics_register_gauge(registry,
        "distric_tracing_drops_total",
        "Cumulative number of spans dropped due to queue overflow",
        NULL, 0, &m->spans_dropped);

    metrics_register_gauge(registry,
        "distric_tracing_sampled_out_total",
        "Cumulative number of spans dropped by the adaptive sampler",
        NULL, 0, &m->spans_sampled_out);

    metrics_register_gauge(registry,
        "distric_tracing_in_backpressure",
        "1 when the tracer is in backpressure mode, 0 otherwise",
        NULL, 0, &m->in_backpressure);

    return DISTRIC_OK;
}

/* ============================================================================
 * Public API — span lifecycle
 * ========================================================================= */

distric_err_t trace_start_span(
    tracer_t*     tracer,
    const char*   operation,
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
    span->start_time_ns = get_time_ns();
    span->status        = SPAN_STATUS_UNSET;
    span->sampled       = true;
    span->_tracer       = tracer;

    *out_span = span;
    return DISTRIC_OK;
}

distric_err_t trace_start_child_span(
    tracer_t*      tracer,
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
    span->start_time_ns  = get_time_ns();
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

/*
 * trace_finish_span:
 *
 * Non-blocking.  If the ring buffer is full the span is counted as a drop
 * and freed immediately — the caller never waits.
 */
void trace_finish_span(tracer_t* tracer, trace_span_t* span) {
    if (!span || !span->sampled || span == &g_sampled_out_span) return;

    span->end_time_ns = get_time_ns();

    if (!tracer) tracer = (tracer_t*)span->_tracer;
    if (!tracer) { free(span); return; }

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
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED,
                          memory_order_release);

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

distric_err_t trace_extract_context(const char*    header,
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
    tracer_t*             tracer,
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
    span->start_time_ns  = get_time_ns();
    span->status         = SPAN_STATUS_UNSET;
    span->sampled        = true;
    span->_tracer        = tracer;

    *out_span = span;
    return DISTRIC_OK;
}

/* ============================================================================
 * Public API — thread-local active span
 * ========================================================================= */

void         trace_set_active_span(trace_span_t* span) { tls_active_span = span; }
trace_span_t* trace_get_active_span(void)               { return tls_active_span; }