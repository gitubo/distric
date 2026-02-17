/**
 * @file distric_obs.h
 * @brief DistriC Observability Library - Complete Public API
 * 
 * Production-ready observability with guaranteed non-blocking operations.
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
    DISTRIC_ERR_TYPE_MISMATCH = -10,
    DISTRIC_ERR_TIMEOUT = -11,
    DISTRIC_ERR_MEMORY = -12,
    DISTRIC_ERR_IO = -13,
    DISTRIC_ERR_INVALID_STATE = -14,
    DISTRIC_ERR_THREAD = -15,
    DISTRIC_ERR_UNAVAILABLE = -16,
    DISTRIC_ERR_REGISTRY_FROZEN = -17,
    DISTRIC_ERR_HIGH_CARDINALITY = -18,
    DISTRIC_ERR_INVALID_LABEL = -19,
    DISTRIC_ERR_BACKPRESSURE = -20,
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

typedef struct {
    char key[MAX_LABEL_KEY_LEN];
    const char **allowed_values;
    uint32_t num_allowed_values;
} metric_label_definition_t;

distric_err_t metrics_init(metrics_registry_t** registry);
void metrics_destroy(metrics_registry_t* registry);

distric_err_t metrics_freeze(metrics_registry_t* registry);

distric_err_t metrics_register_counter(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_definition_t* label_defs,
    size_t label_def_count,
    metric_t** out_metric
);

distric_err_t metrics_register_gauge(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_definition_t* label_defs,
    size_t label_def_count,
    metric_t** out_metric
);

distric_err_t metrics_register_histogram(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_definition_t* label_defs,
    size_t label_def_count,
    metric_t** out_metric
);

distric_err_t metrics_counter_inc_with_labels(
    metric_t* metric,
    const metric_label_t* labels,
    size_t label_count,
    uint64_t value
);

distric_err_t metrics_gauge_set_with_labels(
    metric_t* metric,
    const metric_label_t* labels,
    size_t label_count,
    double value
);

distric_err_t metrics_histogram_observe_with_labels(
    metric_t* metric,
    const metric_label_t* labels,
    size_t label_count,
    double value
);

void metrics_counter_inc(metric_t* metric);
void metrics_counter_add(metric_t* metric, uint64_t value);
void metrics_gauge_set(metric_t* metric, double value);
void metrics_histogram_observe(metric_t* metric, double value);

distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char** out_buffer,
    size_t* out_size
);

/* Value getters - for testing and inspection */
uint64_t metrics_counter_get(metric_t* metric);
double   metrics_gauge_get(metric_t* metric);
uint64_t metrics_histogram_get_count(metric_t* metric);
double   metrics_histogram_get_sum(metric_t* metric);

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

distric_err_t log_init(logger_t** logger, int fd, log_mode_t mode);
void log_destroy(logger_t* logger);
void log_retain(logger_t* logger);
void log_release(logger_t* logger);

distric_err_t log_write(
    logger_t* logger,
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
 * ========================================================================= */

typedef struct tracer_s tracer_t;

typedef struct {
    uint64_t high;
    uint64_t low;
} trace_id_t;

typedef uint64_t span_id_t;

#define TRACE_MAX_SPAN_TAGS        16
#define TRACE_MAX_TAG_KEY_LEN      64
#define TRACE_MAX_TAG_VALUE_LEN    256
#define TRACE_MAX_OPERATION_LEN    128

typedef struct {
    char key[TRACE_MAX_TAG_KEY_LEN];
    char value[TRACE_MAX_TAG_VALUE_LEN];
} span_tag_t;

/* Fully public: users read this in export callbacks */
typedef struct trace_span_s {
    trace_id_t  trace_id;
    span_id_t   span_id;
    span_id_t   parent_span_id;
    char        operation[TRACE_MAX_OPERATION_LEN];
    uint64_t    start_time_ns;
    uint64_t    end_time_ns;
    span_tag_t  tags[TRACE_MAX_SPAN_TAGS];
    size_t      tag_count;
    int         status;   /* span_status_t */
    bool        sampled;
    /* Internal fields - do not access directly */
    void*       _tracer;
} trace_span_t;

typedef enum {
    SPAN_STATUS_UNSET = 0,
    SPAN_STATUS_OK = 1,
    SPAN_STATUS_ERROR = 2,
} span_status_t;

typedef struct {
    trace_id_t trace_id;
    span_id_t span_id;
} trace_context_t;

typedef struct {
    uint32_t always_sample;
    uint32_t always_drop;
    uint32_t backpressure_sample;
    uint32_t backpressure_drop;
} trace_sampling_config_t;

distric_err_t trace_init(
    tracer_t** tracer,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data
);

distric_err_t trace_init_with_sampling(
    tracer_t** tracer,
    const trace_sampling_config_t* sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data
);

void trace_destroy(tracer_t* tracer);
void trace_retain(tracer_t* tracer);
void trace_release(tracer_t* tracer);

distric_err_t trace_start_span(
    tracer_t* tracer,
    const char* operation,
    trace_span_t** out_span
);

distric_err_t trace_start_child_span(
    tracer_t* tracer,
    trace_span_t* parent,
    const char* operation,
    trace_span_t** out_span
);

distric_err_t trace_add_tag(
    trace_span_t* span,
    const char* key,
    const char* value
);

void trace_set_status(trace_span_t* span, span_status_t status);
void trace_finish_span(tracer_t* tracer, trace_span_t* span);

distric_err_t trace_inject_context(
    trace_span_t* span,
    char* header,
    size_t header_size
);

distric_err_t trace_extract_context(
    const char* header,
    trace_context_t* context
);

distric_err_t trace_start_span_from_context(
    tracer_t* tracer,
    const trace_context_t* context,
    const char* operation,
    trace_span_t** out_span
);

void trace_set_active_span(trace_span_t* span);
trace_span_t* trace_get_active_span(void);

/* ============================================================================
 * HEALTH MONITORING
 * ========================================================================= */

typedef struct health_registry_s health_registry_t;
typedef struct health_component_s health_component_t;

typedef enum {
    HEALTH_UP = 0,
    HEALTH_DEGRADED = 1,
    HEALTH_DOWN = 2,
} health_status_t;

distric_err_t health_init(health_registry_t** registry);
void health_destroy(health_registry_t* registry);

distric_err_t health_register_component(
    health_registry_t* registry,
    const char* name,
    health_component_t** out_component
);

distric_err_t health_update_status(
    health_component_t* component,
    health_status_t status,
    const char* message
);

health_status_t health_get_overall_status(health_registry_t* registry);

distric_err_t health_export_json(
    health_registry_t* registry,
    char** out_buffer,
    size_t* out_size
);

const char* health_status_str(health_status_t status);

/* ============================================================================
 * HTTP OBSERVABILITY SERVER
 * ========================================================================= */

typedef struct obs_server_s obs_server_t;

distric_err_t obs_server_init(
    obs_server_t** server,
    uint16_t port,
    metrics_registry_t* metrics,
    health_registry_t* health
);

void obs_server_destroy(obs_server_t* server);
uint16_t obs_server_get_port(obs_server_t* server);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_OBS_H */