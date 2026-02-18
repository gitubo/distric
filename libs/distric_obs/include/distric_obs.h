/**
 * @file distric_obs.h
 * @brief DistriC Observability Library — Public API
 *
 * =============================================================================
 * PRODUCTION INVARIANTS (non-negotiable, enforced regardless of configuration)
 * =============================================================================
 *
 * 1. ALL observability APIs are strictly non-blocking and best-effort.
 *    Every hot-path call completes in O(1) bounded time.
 *    Data may be dropped silently under backpressure — this is by design.
 *
 * 2. Metric registration enforces strict label cardinality rules and cannot
 *    be relaxed or disabled in production builds.  Every label dimension MUST
 *    carry a fully-enumerated allowlist.  Registration fails immediately and
 *    deterministically when any dimension is unbounded or the total cardinality
 *    exceeds MAX_METRIC_CARDINALITY.
 *
 * 3. Tracing automatically reduces overhead under sustained backpressure to
 *    preserve system stability.  Adaptive sampling activates when queue depth
 *    exceeds 75% or when sustained drop rate exceeds threshold; it deactivates
 *    with hysteresis to prevent oscillation.
 *
 * =============================================================================
 * BEST-EFFORT SEMANTICS
 * =============================================================================
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
 * Label cardinality enforcement — STRICT and non-optional in production:
 *
 *   1. Registration time (primary enforcement):
 *      metrics_register_* computes the theoretical combination count for the
 *      provided label_defs.  If any dimension has num_allowed_values == 0 or
 *      allowed_values == NULL the dimension is considered UNBOUNDED and
 *      registration fails with DISTRIC_ERR_HIGH_CARDINALITY.  Registration
 *      also fails when the product of all allowed-value counts exceeds
 *      MAX_METRIC_CARDINALITY (256).
 *
 *      There is NO runtime opt-out for production builds.
 *      Every label dimension MUST have a fully-enumerated allowlist.
 *
 *   2. Update time (defence-in-depth):
 *      metrics_*_with_labels validates the supplied label set against the
 *      stored allowlist.  Any value not in the allowlist → DISTRIC_ERR_INVALID_LABEL.
 *
 *   Debug / test escape hatch:
 *      Define DISTRIC_OBS_ALLOW_OPEN_LABELS at compile time to restore
 *      pre-production behaviour (NULL allowlist = accept any value).
 *      This flag MUST NEVER be defined in production builds.
 * ========================================================================= */

#define MAX_METRICS              1024
#define MAX_METRIC_NAME_LEN      128
#define MAX_METRIC_HELP_LEN      256
#define MAX_LABEL_KEY_LEN        64
#define MAX_LABEL_VALUE_LEN      128
#define MAX_METRIC_LABELS        9
#define MAX_METRIC_CARDINALITY   256

typedef struct metrics_registry_s metrics_registry_t;
typedef struct metric_s           metric_t;

typedef enum {
    METRIC_TYPE_COUNTER   = 0,
    METRIC_TYPE_GAUGE     = 1,
    METRIC_TYPE_HISTOGRAM = 2,
} metric_type_t;

typedef struct {
    char key[MAX_LABEL_KEY_LEN];
    char value[MAX_LABEL_VALUE_LEN];
} metric_label_t;

/**
 * Label dimension definition.
 *
 * REQUIRED FIELDS:
 *   key              — label key (e.g. "method", "status")
 *   allowed_values   — array of permitted values; MUST be non-NULL and
 *                      non-empty in production builds.  Setting this to
 *                      NULL or num_allowed_values to 0 causes registration
 *                      to fail with DISTRIC_ERR_HIGH_CARDINALITY.
 *   num_allowed_values — count of entries in allowed_values; must be >= 1.
 *
 * The product of num_allowed_values across all dimensions for a single
 * metric must not exceed MAX_METRIC_CARDINALITY.
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
 * Enables lock-free export.  Irreversible.  Idempotent.
 */
distric_err_t metrics_freeze(metrics_registry_t* registry);

/**
 * Register a counter metric.
 *
 * label_defs / label_def_count may be NULL / 0 for an unlabelled metric.
 * When non-NULL every dimension must have a fully-enumerated allowlist.
 *
 * Returns DISTRIC_ERR_HIGH_CARDINALITY if any dimension is unbounded or
 * the total combination count exceeds MAX_METRIC_CARDINALITY.
 */
distric_err_t metrics_register_counter(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric
);

distric_err_t metrics_register_gauge(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric
);

distric_err_t metrics_register_histogram(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric
);

/* Unlabelled metric operations — lock-free atomic, ~10-20 ns per call. */
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
 *     2. Check fullness: if head - tail >= RING_BUFFER_SIZE -> drop + return
 *        DISTRIC_ERR_BUFFER_OVERFLOW (non-blocking).
 *     3. Claim slot: atomic_fetch_add(&rb->head, 1) -> unique index.
 *     4. Write entry into slot; publish with SLOT_FILLED store-release.
 *
 *   Consumer (single background flush thread):
 *     1. Load rb->tail to find next slot.
 *     2. Spin <= MAX_SPIN on slot->state until SLOT_FILLED.
 *     3. Write entry to fd; mark slot EMPTY.
 *     4. Advance rb->tail.
 * ========================================================================= */

#define RING_BUFFER_SIZE 8192

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
 *     Enter backpressure mode when >= 5 spans are dropped in a 1-second
 *     rolling window.
 *     Exit only after 3 seconds of zero new drops.
 *
 *   Combined: in_backpressure = (Signal A || Signal B)
 *
 *   Sampling rates:
 *     Normal mode:       always_sample / (always_sample + always_drop)
 *     Backpressure mode: backpressure_sample / (backpressure_sample +
 *                                               backpressure_drop)
 *
 *   Default (trace_init): 100% normal, 10% under backpressure.
 * ========================================================================= */

typedef struct tracer_s tracer_t;

typedef struct { uint64_t high; uint64_t low; } trace_id_t;
typedef uint64_t span_id_t;

#define TRACE_MAX_SPAN_TAGS     16
#define TRACE_MAX_TAG_KEY_LEN   64
#define TRACE_MAX_TAG_VALUE_LEN 256
#define TRACE_MAX_OPERATION_LEN 128
#define MAX_SPANS_BUFFER        1024

typedef struct {
    char key[TRACE_MAX_TAG_KEY_LEN];
    char value[TRACE_MAX_TAG_VALUE_LEN];
} span_tag_t;

typedef struct trace_span_s {
    trace_id_t  trace_id;
    span_id_t   span_id;
    span_id_t   parent_span_id;
    char        operation[TRACE_MAX_OPERATION_LEN];
    uint64_t    start_time_ns;
    uint64_t    end_time_ns;
    span_tag_t  tags[TRACE_MAX_SPAN_TAGS];
    size_t      tag_count;
    bool        sampled;       /* false -> no-op placeholder; do not export  */
    void*       _tracer;       /* internal backlink; do not read or write    */
    int         status;
} trace_span_t;

typedef enum {
    SPAN_STATUS_UNSET = 0,
    SPAN_STATUS_OK    = 1,
    SPAN_STATUS_ERROR = 2,
} span_status_t;

typedef struct {
    trace_id_t trace_id;
    span_id_t  span_id;
} trace_context_t;

/**
 * Sampling configuration.
 * Ratio = always_sample / (always_sample + always_drop)
 * Under backpressure: backpressure_sample / (backpressure_sample + backpressure_drop)
 */
typedef struct {
    uint32_t always_sample;
    uint32_t always_drop;
    uint32_t backpressure_sample;
    uint32_t backpressure_drop;
} trace_sampling_config_t;

/**
 * Snapshot of tracer runtime statistics.
 * All fields are approximate (relaxed reads).
 */
typedef struct {
    uint64_t spans_created;
    uint64_t spans_sampled_in;
    uint64_t spans_sampled_out;
    uint64_t spans_dropped_backpressure;
    uint64_t exports_attempted;
    uint64_t exports_succeeded;
    uint64_t queue_depth;
    uint64_t queue_capacity;
    bool     in_backpressure;
    uint32_t effective_sample_rate_pct;
} tracer_stats_t;

distric_err_t trace_init(
    tracer_t** tracer,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data
);

distric_err_t trace_init_with_sampling(
    tracer_t**                     tracer,
    const trace_sampling_config_t* sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void*                          user_data
);

void trace_retain(tracer_t* tracer);
void trace_release(tracer_t* tracer);
void trace_destroy(tracer_t* tracer);

/** Non-blocking snapshot. Safe to call from any thread. */
void trace_get_stats(tracer_t* tracer, tracer_stats_t* out);

/**
 * Register internal Prometheus gauges for tracer observability.
 * After this call the export loop updates these gauges automatically.
 */
distric_err_t trace_register_metrics(tracer_t* tracer, metrics_registry_t* registry);

distric_err_t trace_start_span(tracer_t* tracer, const char* operation, trace_span_t** out_span);
distric_err_t trace_start_child_span(tracer_t* tracer, trace_span_t* parent, const char* operation, trace_span_t** out_span);
distric_err_t trace_start_span_from_context(tracer_t* tracer, const trace_context_t* ctx, const char* operation, trace_span_t** out_span);
distric_err_t trace_finish_span(tracer_t* tracer, trace_span_t* span);
distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value);
distric_err_t trace_set_status(trace_span_t* span, span_status_t status);
distric_err_t trace_inject_context(trace_span_t* span, char* buf, size_t buf_size);
distric_err_t trace_extract_context(const char* header, trace_context_t* out_ctx);

/* ============================================================================
 * HEALTH MONITORING
 *
 * Component health tracking with overall system status aggregation.
 * Thread-safe status updates; JSON export for readiness/liveness probes.
 * ========================================================================= */

typedef struct health_registry_s health_registry_t;
typedef struct health_component_s health_component_t;

#define MAX_HEALTH_COMPONENTS 64

typedef enum {
    HEALTH_UP       = 0,
    HEALTH_DEGRADED = 1,
    HEALTH_DOWN     = 2,
} health_status_t;

distric_err_t health_init(health_registry_t** registry);
void          health_destroy(health_registry_t* registry);

distric_err_t health_register_component(
    health_registry_t*   registry,
    const char*          name,
    health_component_t** out_component
);

distric_err_t health_update_status(
    health_component_t* component,
    health_status_t     status,
    const char*         message
);

health_status_t health_get_overall_status(health_registry_t* registry);

distric_err_t health_export_json(
    health_registry_t* registry,
    char**             out_json,
    size_t*            out_size
);

/* ============================================================================
 * HTTP SERVER
 *
 * Minimal HTTP server for observability endpoints:
 *   GET /metrics        - Prometheus metrics
 *   GET /health/ready   - Readiness probe (200 if all UP, 503 otherwise)
 *   GET /health/live    - Liveness probe (always 200)
 * ========================================================================= */

typedef struct obs_server_s obs_server_t;

distric_err_t obs_server_init(
    obs_server_t**      server,
    uint16_t            port,
    metrics_registry_t* metrics,
    health_registry_t*  health
);

void obs_server_destroy(obs_server_t* server);

uint16_t obs_server_get_port(obs_server_t* server);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_OBS_H */