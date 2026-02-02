/**
 * @file distric_obs.h
 * @brief DistriC Observability Library - Complete Public API
 * 
 * This is the ONLY header users need to include for full observability support.
 * Provides lock-free metrics, async logging, distributed tracing, and health checks.
 * 
 * Features:
 * - Lock-free metrics (counters, gauges, histograms)
 * - Prometheus-compatible export format
 * - Async JSON logging with ring buffer
 * - Distributed tracing with context propagation
 * - Health check monitoring
 * - HTTP server for /metrics and /health endpoints
 * - Zero external dependencies (except pthread)
 * - <1% CPU overhead
 * 
 * @version 0.2.0
 * @author DistriC Development Team
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
    DISTRIC_OK = 0,
    DISTRIC_ERR_INVALID_ARG = -1,
    DISTRIC_ERR_ALLOC_FAILURE = -2,
    DISTRIC_ERR_BUFFER_OVERFLOW = -3,
    DISTRIC_ERR_REGISTRY_FULL = -4,
    DISTRIC_ERR_NOT_FOUND = -5,
    DISTRIC_ERR_INIT_FAILED = -6,
    DISTRIC_ERR_NO_MEMORY = -7,           
    DISTRIC_ERR_EOF = -8,                 
    DISTRIC_ERR_INVALID_FORMAT = -9,      
    DISTRIC_ERR_TYPE_MISMATCH = -10        
} distric_err_t;

const char* distric_strerror(distric_err_t err);

/* ============================================================================
 * METRICS SYSTEM
 * ========================================================================= */

typedef struct metrics_registry_s metrics_registry_t;
typedef struct metric_s metric_t;

typedef enum {
    METRIC_TYPE_COUNTER,
    METRIC_TYPE_GAUGE,
    METRIC_TYPE_HISTOGRAM,
} metric_type_t;

#define MAX_METRIC_LABELS 8
#define MAX_LABEL_KEY_LEN 64
#define MAX_LABEL_VALUE_LEN 128

typedef struct {
    char key[MAX_LABEL_KEY_LEN];
    char value[MAX_LABEL_VALUE_LEN];
} metric_label_t;

/* Initialize metrics registry */
distric_err_t metrics_init(metrics_registry_t** registry);

/* Destroy metrics registry and free all resources */
void metrics_destroy(metrics_registry_t* registry);

/* Register a counter metric */
distric_err_t metrics_register_counter(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
);

/* Register a gauge metric */
distric_err_t metrics_register_gauge(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
);

/* Register a histogram metric */
distric_err_t metrics_register_histogram(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
);

/* Increment counter by 1 */
void metrics_counter_inc(metric_t* metric);

/* Increment counter by value */
void metrics_counter_add(metric_t* metric, uint64_t value);

/* Set gauge to value */
void metrics_gauge_set(metric_t* metric, double value);

/* Record observation in histogram */
void metrics_histogram_observe(metric_t* metric, double value);

/* Export all metrics in Prometheus text format */
distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char** out_buffer,
    size_t* out_size
);

/* ============================================================================
 * STRUCTURED LOGGING
 * ========================================================================= */

typedef struct logger_s logger_t;

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
} log_level_t;

typedef enum {
    LOG_MODE_SYNC,
    LOG_MODE_ASYNC,
} log_mode_t;

/* Initialize logger with output file descriptor and mode */
distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode);

/* Destroy logger and flush all pending logs */
void log_destroy(logger_t* logger);

/* Write a log entry with key-value pairs (NULL-terminated) */
distric_err_t log_write(
    logger_t* logger,
    log_level_t level,
    const char* component,
    const char* message,
    ...  /* key1, value1, key2, value2, ..., NULL */
);

/* Logging macros */
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
 * ========================================================================= */

typedef struct tracer_s tracer_t;
typedef struct trace_span_s trace_span_t;

/* Trace and span identifiers */
typedef struct {
    uint64_t high;
    uint64_t low;
} trace_id_t;

typedef uint64_t span_id_t;

/* Span status */
typedef enum {
    SPAN_STATUS_UNSET = 0,
    SPAN_STATUS_OK = 1,
    SPAN_STATUS_ERROR = 2,
} span_status_t;

/* Trace context for propagation */
typedef struct {
    trace_id_t trace_id;
    span_id_t span_id;
} trace_context_t;

/* Initialize tracer with export callback */
distric_err_t trace_init(
    tracer_t** tracer, 
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data
);

/* Destroy tracer and flush pending spans */
void trace_destroy(tracer_t* tracer);

/* Start a new root span */
distric_err_t trace_start_span(
    tracer_t* tracer, 
    const char* operation, 
    trace_span_t** out_span
);

/* Start a child span */
distric_err_t trace_start_child_span(
    tracer_t* tracer, 
    trace_span_t* parent,
    const char* operation, 
    trace_span_t** out_span
);

/* Add tag to span */
distric_err_t trace_add_tag(
    trace_span_t* span, 
    const char* key, 
    const char* value
);

/* Set span status */
void trace_set_status(trace_span_t* span, span_status_t status);

/* Finish span and submit for export */
void trace_finish_span(tracer_t* tracer, trace_span_t* span);

/* Context propagation - inject trace context into header string */
distric_err_t trace_inject_context(
    trace_span_t* span, 
    char* header, 
    size_t header_size
);

/* Context propagation - extract trace context from header string */
distric_err_t trace_extract_context(
    const char* header, 
    trace_context_t* context
);

/* Create child span from extracted context */
distric_err_t trace_start_span_from_context(
    tracer_t* tracer, 
    const trace_context_t* context,
    const char* operation,
    trace_span_t** out_span
);

/* Thread-local active span support */
void trace_set_active_span(trace_span_t* span);
trace_span_t* trace_get_active_span(void);

/* ============================================================================
 * HEALTH MONITORING
 * ========================================================================= */

typedef struct health_registry_s health_registry_t;
typedef struct health_component_s health_component_t;  /* Opaque type */

/* Health status */
typedef enum {
    HEALTH_UP = 0,
    HEALTH_DEGRADED = 1,
    HEALTH_DOWN = 2,
} health_status_t;

/* Initialize health registry */
distric_err_t health_init(health_registry_t** registry);

/* Destroy health registry */
void health_destroy(health_registry_t* registry);

/* Register a health check component */
distric_err_t health_register_component(
    health_registry_t* registry,
    const char* name,
    health_component_t** out_component
);

/* Update component health status */
distric_err_t health_update_status(
    health_component_t* component,
    health_status_t status,
    const char* message
);

/* Get overall system health (worst status of all components) */
health_status_t health_get_overall_status(health_registry_t* registry);

/* Export health status as JSON */
distric_err_t health_export_json(
    health_registry_t* registry,
    char** out_buffer,
    size_t* out_size
);

/* Helper: Convert health status to string */
const char* health_status_str(health_status_t status);

/* ============================================================================
 * HTTP OBSERVABILITY SERVER
 * ========================================================================= */

typedef struct obs_server_s obs_server_t;

/* Initialize and start HTTP server for /metrics and /health endpoints */
distric_err_t obs_server_init(
    obs_server_t** server,
    uint16_t port,
    metrics_registry_t* metrics,
    health_registry_t* health
);

/* Stop and destroy HTTP server */
void obs_server_destroy(obs_server_t* server);

/* Get server port (useful if port 0 was used for auto-assignment) */
uint16_t obs_server_get_port(obs_server_t* server);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_OBS_H */