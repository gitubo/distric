/**
 * @file distric_obs.h
 * @brief DistriC Observability Library — Stable Public API (single include)
 *
 * =============================================================================
 * PRODUCTION INVARIANTS
 * =============================================================================
 *
 * 1. ALL hot-path APIs are strictly non-blocking and best-effort.
 *    Every call completes in O(1) bounded time.  Data may be dropped under
 *    backpressure — this is by design and is explicitly signalled via error codes.
 *
 * 2. Metric registration enforces strict label cardinality.  Every label
 *    dimension MUST carry a fully-enumerated allowlist.  Registration fails
 *    immediately when any dimension is unbounded or total cardinality exceeds
 *    the configured cap.
 *
 * 3. Tracing automatically reduces overhead under sustained backpressure.
 *    Adaptive sampling activates on queue fill or sustained drop rate and
 *    deactivates with hysteresis to prevent oscillation.
 *
 * 4. Observability failures MUST NOT affect application correctness.
 *    All error codes are informational; callers may safely ignore them.
 *
 * =============================================================================
 * CONCURRENCY MODEL
 * =============================================================================
 *
 * - Metrics hot-path (inc/set/observe): lock-free C11 atomics. Any thread.
 * - Metrics registration: serialised by internal mutex (rare path).
 * - Logger async: MPSC lock-free ring buffer. Multiple producers, one consumer.
 * - Logger sync: per-write mutex (use only for low-volume / debug scenarios).
 * - Tracing start/finish: lock-free. Export runs on a private background thread.
 * - Health updates: single CAS per component. Any thread.
 * - HTTP server: accepts connections on its own thread; short-lived per-request
 *   tasks copy export data before formatting — server never holds registry locks.
 *
 * =============================================================================
 * API STABILITY POLICY
 * =============================================================================
 *
 * This header is the ONLY stable public interface.  All declarations in
 * include/distric_obs/ subdirectories are INTERNAL and must not be included
 * by consumers.  Breaking changes to this header increment the major version.
 *
 * =============================================================================
 * DROP SIGNAL ERROR CODES (non-fatal)
 * =============================================================================
 *
 *   DISTRIC_ERR_BUFFER_OVERFLOW   ring buffer full; log entry dropped
 *   DISTRIC_ERR_BACKPRESSURE      tracer under load; span sampled out
 *   DISTRIC_ERR_REGISTRY_FULL     metric registration limit reached
 *   DISTRIC_ERR_INVALID_LABEL     label value outside registered allowlist
 *   DISTRIC_ERR_HIGH_CARDINALITY  label combination count exceeds cap
 *   DISTRIC_ERR_REGISTRY_FROZEN   registration attempted after freeze
 *   DISTRIC_ERR_NO_MEMORY         allocation failed; update dropped
 */

#ifndef DISTRIC_OBS_H
#define DISTRIC_OBS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>   /* STDOUT_FILENO */

/* ============================================================================
 * VERSION
 * ========================================================================= */

#define DISTRIC_OBS_VERSION_MAJOR 1
#define DISTRIC_OBS_VERSION_MINOR 0
#define DISTRIC_OBS_VERSION_PATCH 0
#define DISTRIC_OBS_VERSION_STR   "1.0.0"

/* ============================================================================
 * COMPILE-TIME HARD CAPS (absolute upper bounds; override via -D at build time)
 * Runtime config values must not exceed these.
 * ========================================================================= */

#ifndef DISTRIC_MAX_METRICS
#define DISTRIC_MAX_METRICS          1024
#endif

#ifndef DISTRIC_MAX_METRIC_LABELS
#define DISTRIC_MAX_METRIC_LABELS    8
#endif

#ifndef DISTRIC_MAX_METRIC_NAME_LEN
#define DISTRIC_MAX_METRIC_NAME_LEN  128
#endif

#ifndef DISTRIC_MAX_METRIC_HELP_LEN
#define DISTRIC_MAX_METRIC_HELP_LEN  256
#endif

#ifndef DISTRIC_MAX_LABEL_KEY_LEN
#define DISTRIC_MAX_LABEL_KEY_LEN    64
#endif

#ifndef DISTRIC_MAX_LABEL_VALUE_LEN
#define DISTRIC_MAX_LABEL_VALUE_LEN  128
#endif

#ifndef DISTRIC_MAX_METRIC_CARDINALITY
#define DISTRIC_MAX_METRIC_CARDINALITY 10000
#endif

#ifndef DISTRIC_MAX_HEALTH_COMPONENTS
#define DISTRIC_MAX_HEALTH_COMPONENTS 64
#endif

/* Tracing span buffer hard cap (must be power of 2) */
#ifndef DISTRIC_MAX_SPANS_BUFFER
#define DISTRIC_MAX_SPANS_BUFFER     4096
#endif

/* Log ring buffer hard cap (must be power of 2) */
#ifndef DISTRIC_MAX_RING_BUFFER
#define DISTRIC_MAX_RING_BUFFER      32768
#endif

/* Span tag limits exposed here because trace_span_t fields use them */
#define DISTRIC_MAX_SPAN_TAGS        16
#define DISTRIC_MAX_TAG_KEY_LEN      64
#define DISTRIC_MAX_TAG_VALUE_LEN    256
#define DISTRIC_MAX_OPERATION_LEN    128

/* ============================================================================
 * ERROR CODES
 * ========================================================================= */

typedef enum {
    DISTRIC_OK                   =  0,
    DISTRIC_ERR_INVALID_ARG      = -1,
    DISTRIC_ERR_ALLOC_FAILURE    = -2,
    DISTRIC_ERR_INIT_FAILED      = -3,
    DISTRIC_ERR_REGISTRY_FULL    = -4,
    DISTRIC_ERR_NOT_FOUND        = -5,
    DISTRIC_ERR_BUFFER_OVERFLOW  = -6,  /* non-fatal: data dropped   */
    DISTRIC_ERR_BACKPRESSURE     = -7,  /* non-fatal: span dropped   */
    DISTRIC_ERR_INVALID_LABEL    = -8,  /* non-fatal: update dropped */
    DISTRIC_ERR_HIGH_CARDINALITY = -9,  /* fatal at registration     */
    DISTRIC_ERR_REGISTRY_FROZEN  = -10, /* registration post-freeze  */
    DISTRIC_ERR_NO_MEMORY        = -11, /* non-fatal: update dropped */
    DISTRIC_ERR_ALREADY_EXISTS   = -12,
    DISTRIC_ERR_SHUTDOWN         = -13,
    DISTRIC_ERR_IO               = -14, 
    DISTRIC_ERR_INVALID_STATE    = -15, 
    DISTRIC_ERR_THREAD           = -16,    
    DISTRIC_ERR_TIMEOUT          = -17, 
    DISTRIC_ERR_EOF              = -18, /* clean end-of-stream (not an error per se) */
    DISTRIC_ERR_INVALID_FORMAT   = -19, /* malformed wire data / protocol violation  */
    DISTRIC_ERR_TYPE_MISMATCH    = -20, 
    DISTRIC_ERR_UNAVAILABLE      = -21, /* peer/resource exists but is unreachable */
} distric_err_t;

/** Human-readable string for an error code.  Never NULL. */
const char* distric_err_str(distric_err_t err);

/* ============================================================================
 * METRICS
 *
 * Label cardinality rules (non-negotiable in production):
 *   - Every label dimension MUST have a fully-enumerated allowlist.
 *   - NULL or zero-length allowlist == unbounded == registration failure.
 *   - Total cardinality (product of per-dimension sizes) must not exceed
 *     metrics_config_t.max_cardinality (default: DISTRIC_MAX_METRIC_CARDINALITY).
 *
 * Hot-path (inc/set/observe): lock-free atomics, O(1), safe from any thread.
 * Registration path: mutex-serialised, O(n) label validation. Rare.
 * ========================================================================= */

typedef struct metrics_registry_s metrics_registry_t;
typedef struct metric_s           metric_t;

typedef enum {
    METRIC_TYPE_COUNTER   = 0,
    METRIC_TYPE_GAUGE     = 1,
    METRIC_TYPE_HISTOGRAM = 2,
} metric_type_t;

typedef struct {
    char key[DISTRIC_MAX_LABEL_KEY_LEN];
    char value[DISTRIC_MAX_LABEL_VALUE_LEN];
} metric_label_t;

typedef struct {
    char         key[DISTRIC_MAX_LABEL_KEY_LEN];
    const char** allowed_values;
    size_t       num_allowed_values;
} metric_label_definition_t;

/**
 * Runtime configuration for a metrics registry.
 * Passed to metrics_init_with_config(); all fields have safe defaults.
 * Values must not exceed compile-time hard caps.
 */
typedef struct {
    size_t max_metrics;       /**< Max registered metrics. 0 = DISTRIC_MAX_METRICS */
    size_t max_cardinality;   /**< Max label-instance product. 0 = DISTRIC_MAX_METRIC_CARDINALITY */
} metrics_config_t;

/** Initialize with default config. */
distric_err_t metrics_init(metrics_registry_t** registry);

/** Initialize with explicit config.  Fails fast on invalid config. */
distric_err_t metrics_init_with_config(metrics_registry_t**  registry,
                                        const metrics_config_t* config);

void          metrics_destroy(metrics_registry_t* registry);
void          metrics_retain(metrics_registry_t* registry);
void          metrics_release(metrics_registry_t* registry);

/**
 * Freeze the registry: no further registrations allowed.
 * Call before serving /metrics in production.
 */
void metrics_freeze(metrics_registry_t* registry);

/* --- Registration (mutex-serialised; call during init, not on hot path) --- */

distric_err_t metrics_register_counter(
    metrics_registry_t*               registry,
    const char*                       name,
    const char*                       help,
    const metric_label_definition_t*  label_defs,
    size_t                            label_def_count,
    metric_t**                        out_metric
);

distric_err_t metrics_register_gauge(
    metrics_registry_t*               registry,
    const char*                       name,
    const char*                       help,
    const metric_label_definition_t*  label_defs,
    size_t                            label_def_count,
    metric_t**                        out_metric
);

distric_err_t metrics_register_histogram(
    metrics_registry_t*               registry,
    const char*                       name,
    const char*                       help,
    const metric_label_definition_t*  label_defs,
    size_t                            label_def_count,
    metric_t**                        out_metric
);

/* --- Hot-path updates (lock-free; safe from any thread) --- */

void          metrics_counter_inc(metric_t* metric);
void          metrics_counter_add(metric_t* metric, uint64_t value);
distric_err_t metrics_counter_inc_labels(metric_t* metric, const metric_label_t* labels, uint32_t num_labels);
distric_err_t metrics_counter_add_labels(metric_t* metric, const metric_label_t* labels, uint32_t num_labels, uint64_t value);

/* Convenience alias: increment by an explicit delta with label validation.
 * Equivalent to metrics_counter_add_labels. */
static inline distric_err_t
metrics_counter_inc_with_labels(metric_t* m,
                                 const metric_label_t* labels,
                                 uint32_t num_labels,
                                 uint64_t value) {
    return metrics_counter_add_labels(m, labels, num_labels, value);
}

void          metrics_gauge_set(metric_t* metric, double value);
distric_err_t metrics_gauge_set_labels(metric_t* metric, const metric_label_t* labels, uint32_t num_labels, double value);

void          metrics_histogram_observe(metric_t* metric, double value);
distric_err_t metrics_histogram_observe_labels(metric_t* metric, const metric_label_t* labels, uint32_t num_labels, double value);

/* --- Read-back (approximate; for testing / debugging) --- */

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
 * LOGGING
 *
 * Async mode (recommended for production):
 *   - MPSC lock-free ring buffer; producer never blocks.
 *   - Background thread drains buffer and writes to fd.
 *   - Returns DISTRIC_ERR_BUFFER_OVERFLOW (non-fatal) if ring is full.
 *
 * Sync mode (debug / low-volume only):
 *   - Direct write on caller thread; may block on slow I/O.
 *
 * Safe API (log_write_kv): explicit kv array, no variadic NULL termination.
 * Variadic API (LOG_* macros): flexible but requires NULL sentinel — marked
 * "advanced/unsafe" for production use.
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
    LOG_MODE_SYNC,   /**< Direct write on caller thread. May block on I/O.       */
    LOG_MODE_ASYNC,  /**< Non-blocking ring buffer + background flush. Preferred. */
} log_mode_t;

/** Key-value pair for the safe logging API. */
typedef struct {
    const char* key;    /**< Field name.  Must not be NULL. */
    const char* value;  /**< Field value. NULL is formatted as empty string. */
} log_kv_t;

/**
 * Runtime configuration for the logger.
 * Pass to log_init_with_config().  Zero-value fields use safe defaults.
 */
typedef struct {
    int        fd;                   /**< Output file descriptor (required)            */
    log_mode_t mode;                 /**< Sync or async (default: LOG_MODE_ASYNC)      */
    size_t     ring_buffer_capacity; /**< Slots in async ring (0 = 8192; must be 2^n) */
    size_t     max_entry_bytes;      /**< Max formatted entry size (0 = 4096)          */
} logging_config_t;

/** Initialize with default config. */
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode);

/** Initialize with explicit config.  Fails fast on invalid config. */
distric_err_t log_init_with_config(logger_t** logger, const logging_config_t* config);

void log_destroy(logger_t* logger);
void log_retain(logger_t* logger);
void log_release(logger_t* logger);

/**
 * [SAFE API] Write a structured JSON log entry using an explicit kv array.
 * No variadic arguments; no NULL sentinel required.
 * Best-effort: returns DISTRIC_ERR_BUFFER_OVERFLOW if async buffer is full.
 */
distric_err_t log_write_kv(
    logger_t*       logger,
    log_level_t     level,
    const char*     component,
    const char*     message,
    const log_kv_t* kv_pairs,
    size_t          kv_count
);

/**
 * [ADVANCED / UNSAFE API] Write a structured log entry.
 * Variadic args: alternating (const char* key, const char* value) pairs,
 * terminated by a NULL key.  Incorrect NULL termination or key/value mismatch
 * causes silent log corruption.  Prefer log_write_kv() in new code.
 */
distric_err_t log_write(
    logger_t*   logger,
    log_level_t level,
    const char* component,
    const char* message,
    ...         /* (key, value, ..., NULL) */
);

/**
 * Register internal backpressure gauges with a metrics registry.
 * After this call the logger updates them automatically.
 *   distric_internal_log_drops_total   (gauge) cumulative dropped entries
 *   distric_internal_log_ring_fill_pct (gauge) 0-100 ring fill percentage
 */
distric_err_t log_register_metrics(logger_t* logger, metrics_registry_t* registry);

/* Convenience macros — use log_write_kv for new production code */
#define LOG_DEBUG(l,c,m,...) log_write((l),LOG_LEVEL_DEBUG,(c),(m),__VA_ARGS__)
#define LOG_INFO(l,c,m,...)  log_write((l),LOG_LEVEL_INFO, (c),(m),__VA_ARGS__)
#define LOG_WARN(l,c,m,...)  log_write((l),LOG_LEVEL_WARN, (c),(m),__VA_ARGS__)
#define LOG_ERROR(l,c,m,...) log_write((l),LOG_LEVEL_ERROR,(c),(m),__VA_ARGS__)
#define LOG_FATAL(l,c,m,...) log_write((l),LOG_LEVEL_FATAL,(c),(m),__VA_ARGS__)

/** Returns the count of entries dropped because they exceeded max_entry_bytes. */
uint64_t log_get_oversized_drops(const logger_t* logger);

/** Returns true if the async flush thread is alive and making progress. */
bool log_is_exporter_healthy(const logger_t* logger);

/* ============================================================================
 * DISTRIBUTED TRACING
 *
 * Lifecycle:
 *   trace_init / trace_init_with_config -> trace_register_metrics (optional)
 *   -> [ trace_start_span / trace_finish_span ]* -> trace_destroy
 *
 * Sampling policy (two-signal adaptive):
 *   Signal A (queue fill): enter at 75%, exit at 50% (hysteresis).
 *   Signal B (drop rate):  enter when ≥5 drops/s, exit after 3s of zero drops.
 *   Combined: in_backpressure = A || B
 *
 * Hot-path invariants:
 *   - trace_start_span / trace_finish_span are lock-free from any thread.
 *   - The exporter thread is the ONLY writer of sampling policy state.
 *   - cached_time_ns is refreshed by the exporter thread (<1ms stale).
 *     Producers read it via relaxed atomic load to avoid syscall on hot path.
 * ========================================================================= */

typedef struct tracer_s tracer_t;

typedef struct { uint64_t high; uint64_t low; } trace_id_t;
typedef uint64_t span_id_t;

typedef struct {
    char key[DISTRIC_MAX_TAG_KEY_LEN];
    char value[DISTRIC_MAX_TAG_VALUE_LEN];
} span_tag_t;

typedef struct trace_span_s {
    trace_id_t  trace_id;
    span_id_t   span_id;
    span_id_t   parent_span_id;
    char        operation[DISTRIC_MAX_OPERATION_LEN];
    uint64_t    start_time_ns;
    uint64_t    end_time_ns;
    span_tag_t  tags[DISTRIC_MAX_SPAN_TAGS];
    size_t      tag_count;
    bool        sampled;       /**< false → no-op placeholder; do not export */
    void*       _tracer;       /**< internal backlink; do NOT read or write   */
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

/** Sampling policy configuration (immutable after init). */
typedef struct {
    uint32_t always_sample;        /**< Normal mode: keep N per N+D */
    uint32_t always_drop;          /**< Normal mode: drop D per N+D */
    uint32_t backpressure_sample;  /**< Backpressure mode numerator  */
    uint32_t backpressure_drop;    /**< Backpressure mode denominator */
} trace_sampling_config_t;

/**
 * Runtime configuration for a tracer.
 * All fields have safe defaults when zero-initialised.
 */
typedef struct {
    trace_sampling_config_t sampling;          /**< Sampling policy (immutable after init)    */
    size_t                  buffer_capacity;   /**< Span buffer slots (0=1024; must be 2^n)   */
    uint32_t                export_interval_ms;/**< Export batch interval (0=5000ms)           */
    void (*export_callback)(trace_span_t*, size_t, void*); /**< Required                      */
    void*                   user_data;
} tracer_config_t;

/** Snapshot of tracer runtime statistics. All fields are approximate. */
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

/** Initialize with default sampling config (100% normal, 10% under backpressure). */
distric_err_t trace_init(
    tracer_t** tracer,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data
);

/** Initialize with explicit sampling config (deprecated; prefer trace_init_with_config). */
distric_err_t trace_init_with_sampling(
    tracer_t**                     tracer,
    const trace_sampling_config_t* sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void*                          user_data
);

/** Initialize with full config struct.  Validates all fields; fails fast. */
distric_err_t trace_init_with_config(
    tracer_t**           tracer,
    const tracer_config_t* config
);

void trace_retain(tracer_t* tracer);
void trace_release(tracer_t* tracer);
void trace_destroy(tracer_t* tracer);

/** Non-blocking statistics snapshot.  Safe from any thread. */
void trace_get_stats(tracer_t* tracer, tracer_stats_t* out);

/**
 * Register internal Prometheus metrics with a registry.
 * The exporter thread updates them automatically after this call.
 *   distric_internal_tracer_queue_depth       (gauge)
 *   distric_internal_tracer_sample_rate_pct   (gauge)
 *   distric_internal_tracer_spans_dropped     (gauge)
 *   distric_internal_tracer_spans_sampled_out (gauge)
 *   distric_internal_tracer_in_backpressure   (gauge)
 */
distric_err_t trace_register_metrics(tracer_t* tracer, metrics_registry_t* registry);
/** Returns true if the tracer exporter thread is alive and making progress. */
bool trace_is_exporter_healthy(const tracer_t* tracer);

distric_err_t trace_start_span(tracer_t* tracer, const char* operation, trace_span_t** out_span);
distric_err_t trace_start_child_span(tracer_t* tracer, trace_span_t* parent, const char* operation, trace_span_t** out_span);
distric_err_t trace_start_span_from_context(tracer_t* tracer, const trace_context_t* ctx, const char* operation, trace_span_t** out_span);
distric_err_t trace_finish_span(tracer_t* tracer, trace_span_t* span);
distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value);
distric_err_t trace_set_status(trace_span_t* span, span_status_t status);
distric_err_t trace_inject_context(trace_span_t* span, char* buf, size_t buf_size);
distric_err_t trace_extract_context(const char* header, trace_context_t* out_ctx);

/**
 * Thread-local active span accessors.
 *
 * trace_set_active_span sets the calling thread's "current" span context.
 * trace_get_active_span retrieves it (returns NULL if none set).
 *
 * Useful for implicit context propagation — set on span start,
 * clear on span finish. These are purely advisory; the library never
 * reads tl_active_span internally.
 */
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

/**
 * Export health status as JSON.  Caller must free(*out_json).
 */
distric_err_t health_export_json(
    health_registry_t* registry,
    char**             out_json,
    size_t*            out_size
);

const char* health_status_str(health_status_t status);

/* ============================================================================
 * HTTP SERVER
 *
 * Minimal, single-purpose observability HTTP server.
 * Endpoints:
 *   GET /metrics        Prometheus metrics (text/plain; version=0.0.4)
 *   GET /health/ready   200 if all components UP, 503 otherwise
 *   GET /health/live    Always 200
 *
 * Security:
 *   - Per-request read/write timeouts enforced (default 5s / 10s).
 *   - Request bodies are rejected; max request line 4096 bytes.
 *   - Response data is always copied before formatting (no registry locks
 *     held while writing to the network).
 *   - Path traversal (../) rejected with 400.
 *   - Only GET method accepted; others receive 405.
 *
 * Concurrency:
 *   - Accepts on dedicated thread.
 *   - Each request handled inline; no thread pool (observability only).
 *   - Exporter data is copied atomically before connection handling.
 * ========================================================================= */

typedef struct obs_server_s obs_server_t;

/**
 * Runtime configuration for the HTTP server.
 */
typedef struct {
    uint16_t            port;             /**< 0 = auto-assign                          */
    metrics_registry_t* metrics;          /**< Required                                 */
    health_registry_t*  health;           /**< Required                                 */
    uint32_t            read_timeout_ms;  /**< Per-request read timeout  (0 = 5000ms)   */
    uint32_t            write_timeout_ms; /**< Per-request write timeout (0 = 10000ms)  */
    size_t              max_response_bytes;/**< Max response body bytes  (0 = 4MB)       */
} obs_server_config_t;

/** Initialize with minimal config. */
distric_err_t obs_server_init(
    obs_server_t**      server,
    uint16_t            port,
    metrics_registry_t* metrics,
    health_registry_t*  health
);

/** Initialize with full config struct. */
distric_err_t obs_server_init_with_config(
    obs_server_t**             server,
    const obs_server_config_t* config
);

void     obs_server_destroy(obs_server_t* server);
uint16_t obs_server_get_port(obs_server_t* server);

/** Register internal HTTP server Prometheus metrics with a registry. */
distric_err_t obs_server_register_internal_metrics(obs_server_t* server,
                                                    metrics_registry_t* registry);
                                                    
#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_OBS_H */