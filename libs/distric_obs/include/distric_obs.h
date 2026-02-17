/**
 * @file distric_obs.h
 * @brief DistriC Observability Library — Public API
 *
 * =============================================================================
 * BEST-EFFORT SEMANTICS
 * =============================================================================
 *
 * ALL observability APIs in this library are strictly best-effort and
 * non-blocking.  This is a deliberate design constraint, not a limitation.
 *
 * Guarantees:
 *   - Every API call completes in bounded, O(1) time on the hot path.
 *   - No API call will block, spin-wait on caller, or sleep.
 *   - When internal resources are exhausted, data is dropped and an error
 *     code is returned.  Applications MUST NOT treat observability errors as
 *     fatal — the subsystem continues operating after any error.
 *
 * Drop signals (non-fatal):
 *   DISTRIC_ERR_BUFFER_OVERFLOW   ring buffer full; log entry dropped
 *   DISTRIC_ERR_BACKPRESSURE      tracer under load; span sampled out
 *   DISTRIC_ERR_REGISTRY_FULL     metric registration limit reached
 *   DISTRIC_ERR_INVALID_LABEL     label violated the registered allowlist
 *   DISTRIC_ERR_HIGH_CARDINALITY  label combination count exceeds cap
 *   DISTRIC_ERR_REGISTRY_FROZEN   registration attempted after freeze
 *   DISTRIC_ERR_NO_MEMORY         instance allocation failed; update dropped
 *
 * Concurrency model:
 *   - All APIs are safe to call from multiple threads simultaneously.
 *   - Logger async mode: MPSC ring buffer.  Multiple producers claim slots
 *     via atomic fetch-add; one consumer drains them.
 *   - Metrics hot-path (inc/set/observe): lock-free atomic operations.
 *   - Metrics registration: serialised by a mutex (registration is rare).
 *   - Tracing: lock-free span creation + finish; export runs on a background
 *     thread.  Adaptive sampling activates automatically under backpressure.
 *
 * =============================================================================
 */

#ifndef DISTRIC_OBS_H
#define DISTRIC_OBS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ERROR HANDLING
 * ========================================================================= */

typedef enum {
    DISTRIC_OK                  =  0,
    DISTRIC_ERR_INVALID_ARG     = -1,
    DISTRIC_ERR_ALLOC_FAILURE   = -2,
    DISTRIC_ERR_BUFFER_OVERFLOW = -3,  /* ring buffer full; entry dropped       */
    DISTRIC_ERR_REGISTRY_FULL   = -4,  /* metric cap reached                    */
    DISTRIC_ERR_NOT_FOUND       = -5,
    DISTRIC_ERR_INIT_FAILED     = -6,
    DISTRIC_ERR_NO_MEMORY       = -7,  /* instance alloc failed; update dropped */
    DISTRIC_ERR_EOF             = -8,
    DISTRIC_ERR_INVALID_FORMAT  = -9,
    DISTRIC_ERR_TYPE_MISMATCH   = -10,
    DISTRIC_ERR_TIMEOUT         = -11,
    DISTRIC_ERR_MEMORY          = -12,
    DISTRIC_ERR_IO              = -13,
    DISTRIC_ERR_INVALID_STATE   = -14,
    DISTRIC_ERR_THREAD          = -15,
    DISTRIC_ERR_UNAVAILABLE     = -16,
    DISTRIC_ERR_REGISTRY_FROZEN = -17, /* registration after freeze             */
    DISTRIC_ERR_HIGH_CARDINALITY= -18, /* label combo count exceeds cap         */
    DISTRIC_ERR_INVALID_LABEL   = -19, /* label not in allowlist                */
    DISTRIC_ERR_BACKPRESSURE    = -20, /* tracer buffer pressure; span dropped  */
} distric_err_t;

const char* distric_strerror(distric_err_t err);

/* ============================================================================
 * METRICS SYSTEM
 *
 * Label cardinality enforcement — STRICT by default:
 *
 *   Labels are validated against per-metric allowlists at TWO points:
 *
 *   1. Registration time (primary enforcement):
 *      metrics_register_* computes the theoretical combination count for the
 *      provided label_defs.  If any dimension has num_allowed_values == 0 or
 *      allowed_values == NULL the dimension is considered UNBOUNDED and
 *      registration fails with DISTRIC_ERR_HIGH_CARDINALITY.  Registration
 *      also fails when the product of all allowed-value counts exceeds
 *      MAX_METRIC_CARDINALITY (256).
 *
 *      There is NO runtime opt-out for production builds.  The previous
 *      "allowed_values=NULL permits any value" escape hatch has been removed.
 *      Every label dimension MUST have a fully-enumerated allowlist.
 *
 *   2. Update time (defence-in-depth):
 *      metrics_counter_inc_with_labels / metrics_gauge_set_with_labels /
 *      metrics_histogram_observe_with_labels each call validate_labels().
 *      Any update using a key/value outside the allowlist returns
 *      DISTRIC_ERR_INVALID_LABEL and the update is silently dropped.
 *
 *   Debug / test builds only:
 *      Compile with -DDISTRIC_OBS_ALLOW_OPEN_LABELS to restore the pre-
 *      production behaviour where a NULL allowlist accepts any value.
 *      This flag MUST NOT be set in production builds.
 *
 *   Metrics registered with no label_defs (NULL, 0) accept only unlabelled
 *   updates (label_count must be 0 at update time).
 * ========================================================================= */

typedef struct metrics_registry_s metrics_registry_t;
typedef struct metric_s metric_t;

typedef enum {
    METRIC_TYPE_COUNTER,
    METRIC_TYPE_GAUGE,
    METRIC_TYPE_HISTOGRAM,
} metric_type_t;

#define MAX_METRIC_LABELS     8
#define MAX_LABEL_KEY_LEN     64
#define MAX_LABEL_VALUE_LEN   128

/**
 * Hard cap on distinct label value combinations per metric.
 * Registration fails with DISTRIC_ERR_HIGH_CARDINALITY when exceeded.
 * Protects Prometheus from cardinality explosions.
 */
#define MAX_METRIC_CARDINALITY 256

typedef struct {
    char key[MAX_LABEL_KEY_LEN];
    char value[MAX_LABEL_VALUE_LEN];
} metric_label_t;

/**
 * Allowlist definition for one label dimension.
 *
 * REQUIRED FIELDS:
 *   key              — label key (e.g. "method", "status")
 *   allowed_values   — array of permitted values; MUST be non-NULL and
 *                      non-empty in production builds.  Setting this to
 *                      NULL or num_allowed_values to 0 causes registration
 *                      to fail with DISTRIC_ERR_HIGH_CARDINALITY.
 *   num_allowed_values — count of entries in allowed_values; must be ≥ 1.
 *
 * The product of num_allowed_values across all dimensions for a single
 * metric must not exceed MAX_METRIC_CARDINALITY.
 *
 * EXAMPLE — valid definition:
 *   const char* methods[] = {"GET", "POST", "PUT", "DELETE"};
 *   metric_label_definition_t def = {
 *       .key              = "method",
 *       .allowed_values   = methods,
 *       .num_allowed_values = 4,
 *   };
 */
typedef struct {
    char          key[MAX_LABEL_KEY_LEN];
    const char**  allowed_values;
    uint32_t      num_allowed_values;
} metric_label_definition_t;

distric_err_t metrics_init(metrics_registry_t** registry);
void          metrics_destroy(metrics_registry_t* registry);
void          metrics_retain(metrics_registry_t* registry);
void          metrics_release(metrics_registry_t* registry);

/**
 * Freeze the registry to prevent further metric registration.
 * Enables lock-free export.  Irreversible.  Idempotent (returns
 * DISTRIC_ERR_REGISTRY_FROZEN if already frozen — not fatal).
 */
distric_err_t metrics_freeze(metrics_registry_t* registry);

/**
 * Register a counter metric.
 *
 * label_defs / label_def_count may be NULL / 0 for an unlabelled metric.
 * When non-NULL every dimension must have a fully-enumerated allowlist.
 *
 * Thread-safe.  Returns DISTRIC_ERR_HIGH_CARDINALITY if any dimension is
 * unbounded or the total combination count exceeds MAX_METRIC_CARDINALITY.
 */
distric_err_t metrics_register_counter(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric
);

/** Register a gauge metric.  Same semantics as metrics_register_counter. */
distric_err_t metrics_register_gauge(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric
);

/** Register a histogram metric.  Same semantics as metrics_register_counter. */
distric_err_t metrics_register_histogram(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric
);

/* Unlabelled metric operations — lock-free atomic, ~10–20 ns per call. */
void metrics_counter_inc(metric_t* metric);
void metrics_counter_add(metric_t* metric, uint64_t value);
void metrics_gauge_set(metric_t* metric, double value);
void metrics_histogram_observe(metric_t* metric, double value);

/* Labelled metric operations — validate allowlist, then lock-free update. */
distric_err_t metrics_counter_inc_with_labels(
    metric_t*             metric,
    const metric_label_t* labels,
    size_t                label_count,
    uint64_t              value
);
distric_err_t metrics_gauge_set_with_labels(
    metric_t*             metric,
    const metric_label_t* labels,
    size_t                label_count,
    double                value
);
distric_err_t metrics_histogram_observe_with_labels(
    metric_t*             metric,
    const metric_label_t* labels,
    size_t                label_count,
    double                value
);

/* Read-side helpers (tests / HTTP export path). */
uint64_t metrics_counter_get(metric_t* metric);
double   metrics_gauge_get(metric_t* metric);
uint64_t metrics_histogram_get_count(metric_t* metric);
double   metrics_histogram_get_sum(metric_t* metric);

/**
 * Render all registered metrics as Prometheus text format.
 * Caller must free(*out_buffer).
 * Thread-safe after metrics_freeze().
 */
distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char**              out_buffer,
    size_t*             out_size
);

/* ============================================================================
 * LOGGING SYSTEM
 *
 * Ring-buffer model (async mode):
 *   Producers:
 *     1. Format JSON into thread-local buffer (no heap allocation).
 *     2. Check fullness: if head - tail >= RING_BUFFER_SIZE → drop + return
 *        DISTRIC_ERR_BUFFER_OVERFLOW (non-blocking).
 *     3. Claim slot: atomic_fetch_add(&rb->head, 1, acq_rel) → unique index.
 *     4. Write entry into slot (non-atomic field writes are safe; producer
 *        has exclusive ownership until the SLOT_FILLED publish).
 *     5. Publish: atomic_store(&slot->state, SLOT_FILLED, release).
 *
 *   Consumer (single background flush thread):
 *     1. Load rb->tail (acquire) to find next slot.
 *     2. Spin ≤ MAX_SPIN on slot->state until SLOT_FILLED (acquire load).
 *     3. Write entry to fd; mark slot EMPTY (release store).
 *     4. Advance rb->tail (release store) to reclaim the slot.
 * ========================================================================= */

typedef struct logger_s logger_t;

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
} log_level_t;

typedef enum {
    LOG_MODE_SYNC,  /* Direct write on caller thread.  May block on I/O. */
    LOG_MODE_ASYNC, /* Non-blocking ring buffer + background flush.       */
} log_mode_t;

distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode);
void log_destroy(logger_t* logger);
void log_retain(logger_t* logger);
void log_release(logger_t* logger);

/**
 * Write a structured JSON log entry.
 * Variadic: alternating (const char* key, const char* value) pairs, NULL-terminated.
 * Best-effort: DISTRIC_ERR_BUFFER_OVERFLOW if async buffer is full (entry dropped).
 */
distric_err_t log_write(
    logger_t*   logger,
    log_level_t level,
    const char* component,
    const char* message,
    ...
);

#define LOG_DEBUG(logger, component, message, ...) \
    log_write(logger, LOG_LEVEL_DEBUG, component, message, ##__VA_ARGS__, NULL)
#define LOG_INFO(logger, component, message, ...) \
    log_write(logger, LOG_LEVEL_INFO, component, message, ##__VA_ARGS__, NULL)
#define LOG_WARN(logger, component, message, ...) \
    log_write(logger, LOG_LEVEL_WARN, component, message, ##__VA_ARGS__, NULL)
#define LOG_ERROR(logger, component, message, ...) \
    log_write(logger, LOG_LEVEL_ERROR, component, message, ##__VA_ARGS__, NULL)
#define LOG_FATAL(logger, component, message, ...) \
    log_write(logger, LOG_LEVEL_FATAL, component, message, ##__VA_ARGS__, NULL)

/* ============================================================================
 * DISTRIBUTED TRACING
 *
 * Adaptive sampling — two-signal backpressure model:
 *
 *   Signal A — queue fill percentage:
 *     Enter backpressure mode when buffer > 75% full.
 *     Exit  backpressure mode when buffer < 50% full.
 *     Hysteresis prevents oscillation during transient bursts.
 *
 *   Signal B — sustained drop rate:
 *     Enter backpressure mode when ≥ 5 spans are dropped in a 1-second
 *     rolling window (indicating a slow or stalled exporter even before
 *     the queue fills completely).
 *     Exit only after 3 seconds of zero new drops.
 *
 *   Combined: in_backpressure = (Signal A || Signal B)
 *
 *   Sampling rates:
 *     Normal mode:      always_sample / (always_sample + always_drop)
 *     Backpressure mode: backpressure_sample / (backpressure_sample +
 *                                               backpressure_drop)
 *
 *   Default (trace_init): 100% normal, 10% under backpressure.
 *   Sampling decision is head-based (made at span creation).
 *   Unsampled spans use a static no-op placeholder; all operations on them
 *   are safe and approximately free (~50 ns).
 *
 * Observability of the tracer itself:
 *   Call trace_get_stats() for a non-blocking snapshot.
 *   Call trace_register_metrics() to expose live Prometheus gauges.
 * ========================================================================= */

typedef struct tracer_s tracer_t;

typedef struct { uint64_t high; uint64_t low; } trace_id_t;
typedef uint64_t span_id_t;

#define TRACE_MAX_SPAN_TAGS     16
#define TRACE_MAX_TAG_KEY_LEN   64
#define TRACE_MAX_TAG_VALUE_LEN 256
#define TRACE_MAX_OPERATION_LEN 128

typedef struct {
    char key[TRACE_MAX_TAG_KEY_LEN];
    char value[TRACE_MAX_TAG_VALUE_LEN];
} span_tag_t;

/**
 * A trace span.  Fully public so export callbacks can read all fields.
 * NEVER write to fields directly — use the trace_* API.
 * The _tracer field is an internal backlink — do not access it.
 */
typedef struct trace_span_s {
    trace_id_t  trace_id;
    span_id_t   span_id;
    span_id_t   parent_span_id;
    char        operation[TRACE_MAX_OPERATION_LEN];
    uint64_t    start_time_ns;
    uint64_t    end_time_ns;
    span_tag_t  tags[TRACE_MAX_SPAN_TAGS];
    size_t      tag_count;
    bool        sampled;       /* false → no-op placeholder; do not export  */
    /* ---- internal ---- */
    void*       _tracer;       /* backlink; do not read or write            */

    /* Span status */
    int         status;        /* span_status_t cast to int for padding */
} trace_span_t;

typedef enum {
    SPAN_STATUS_UNSET = 0,
    SPAN_STATUS_OK    = 1,
    SPAN_STATUS_ERROR = 2,
} span_status_t;

/** Context for cross-service propagation. */
typedef struct {
    trace_id_t trace_id;
    span_id_t  span_id;
} trace_context_t;

/**
 * Sampling configuration.
 *
 * Sampling ratio = always_sample / (always_sample + always_drop)
 *
 * Under backpressure (buffer > 75% or sustained drops detected):
 *   ratio = backpressure_sample / (backpressure_sample + backpressure_drop)
 */
typedef struct {
    uint32_t always_sample;       /* numerator for normal sampling    */
    uint32_t always_drop;         /* denominator complement           */
    uint32_t backpressure_sample; /* numerator for backpressure mode  */
    uint32_t backpressure_drop;   /* denominator complement           */
} trace_sampling_config_t;

/**
 * Snapshot of tracer runtime statistics.
 * All fields are approximate (relaxed reads).  Suitable for dashboards,
 * alerting, and inclusion in log lines.
 */
typedef struct {
    uint64_t spans_created;               /* total span creation attempts       */
    uint64_t spans_sampled_in;            /* spans that passed the sampler      */
    uint64_t spans_sampled_out;           /* spans dropped by the sampler       */
    uint64_t spans_dropped_backpressure;  /* spans dropped due to full buffer   */
    uint64_t exports_attempted;           /* export callback invocations        */
    uint64_t exports_succeeded;           /* export callbacks that returned     */
    uint64_t queue_depth;                 /* current spans pending export       */
    uint64_t queue_capacity;              /* total ring-buffer capacity         */
    bool     in_backpressure;             /* true when any backpressure signal  */
    uint32_t effective_sample_rate_pct;   /* 0-100 given current mode           */
} tracer_stats_t;

/* ---- Lifecycle ---- */

/**
 * Initialise a tracer with default sampling (100% normal, 10% backpressure).
 * export_callback is invoked from a background thread; it must be non-blocking
 * or bounded in time to avoid starving the export loop.
 */
distric_err_t trace_init(
    tracer_t** tracer,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data
);

/** Initialise with explicit sampling configuration. */
distric_err_t trace_init_with_sampling(
    tracer_t**                     tracer,
    const trace_sampling_config_t* sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void*                          user_data
);

void trace_retain(tracer_t* tracer);
void trace_release(tracer_t* tracer);
void trace_destroy(tracer_t* tracer);

/* ---- Stats ---- */

/**
 * Non-blocking snapshot of tracing statistics.
 * Safe to call from any thread at any time.
 * Fields are approximate (relaxed atomic reads).
 */
void trace_get_stats(tracer_t* tracer, tracer_stats_t* out);

/**
 * Register five internal Prometheus gauges in the provided registry and wire
 * them to the tracer's counters.  After this call the export loop updates
 * these gauges automatically.
 *
 * Gauges registered:
 *   distric_tracing_queue_depth      — buffer occupancy
 *   distric_tracing_sample_rate_pct  — effective sampling rate [0-100]
 *   distric_tracing_drops_total      — cumulative overflow drops
 *   distric_tracing_sampled_out_total — cumulative sampler drops
 *   distric_tracing_in_backpressure  — 0 or 1
 *
 * Must be called from a single thread before concurrent use.
 */
distric_err_t trace_register_metrics(tracer_t*           tracer,
                                      metrics_registry_t* registry);

/* ---- Span lifecycle ---- */

/** Create a root span.  Non-blocking.  Returns a sampled-out no-op if the
 *  sampler drops it or the buffer is full. */
distric_err_t trace_start_span(
    tracer_t*     tracer,
    const char*   operation,
    trace_span_t** out_span
);

/** Create a child span linked to a parent. */
distric_err_t trace_start_child_span(
    tracer_t*     tracer,
    trace_span_t* parent,
    const char*   operation,
    trace_span_t** out_span
);

/** Add a key/value tag to a span.  No-op on sampled-out spans. */
distric_err_t trace_add_tag(trace_span_t* span,
                              const char*   key,
                              const char*   value);

/** Set span status.  No-op on sampled-out spans. */
void trace_set_status(trace_span_t* span, span_status_t status);

/**
 * Finalise a span and enqueue it for export.  Non-blocking.
 * If the export buffer is full the span is counted as a drop and freed.
 * Frees the span allocation; the pointer is invalid after this call.
 */
void trace_finish_span(tracer_t* tracer, trace_span_t* span);

/* ---- Context propagation ---- */

/** Serialise a span context into a W3C traceparent header. */
distric_err_t trace_inject_context(trace_span_t*  span,
                                    char*           header,
                                    size_t          header_size);

/** Parse a W3C traceparent header into a context struct. */
distric_err_t trace_extract_context(const char*    header,
                                     trace_context_t* context);

/** Start a span that continues a remote trace context. */
distric_err_t trace_start_span_from_context(
    tracer_t*              tracer,
    const trace_context_t* context,
    const char*            operation,
    trace_span_t**         out_span
);

/* ---- Thread-local active span ---- */
void          trace_set_active_span(trace_span_t* span);
trace_span_t* trace_get_active_span(void);

/* ============================================================================
 * HEALTH MONITORING
 * ========================================================================= */

typedef struct health_registry_s  health_registry_t;
typedef struct health_component_s health_component_t;

typedef enum {
    HEALTH_UP       = 0,
    HEALTH_DEGRADED = 1,
    HEALTH_DOWN     = 2,
} health_status_t;

distric_err_t   health_init(health_registry_t** registry);
void            health_destroy(health_registry_t* registry);
distric_err_t   health_register_component(health_registry_t* registry,
                                          const char* name,
                                          health_component_t** out_component);
distric_err_t   health_update_status(health_component_t* component,
                                     health_status_t status,
                                     const char* message);
health_status_t health_get_overall_status(health_registry_t* registry);
distric_err_t   health_export_json(health_registry_t* registry,
                                   char** out_buffer,
                                   size_t* out_size);
const char*     health_status_str(health_status_t status);

/* ============================================================================
 * HTTP OBSERVABILITY SERVER
 *   GET /metrics       Prometheus text format
 *   GET /health/live   Liveness  {"status":"UP"}
 *   GET /health/ready  Readiness health_export_json output
 * ========================================================================= */

typedef struct obs_server_s obs_server_t;

distric_err_t obs_server_init(obs_server_t**      server,
                               uint16_t             port,
                               metrics_registry_t*  metrics,
                               health_registry_t*   health);
void          obs_server_destroy(obs_server_t* server);
uint16_t      obs_server_get_port(obs_server_t* server);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_OBS_H */