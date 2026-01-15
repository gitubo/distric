/**
 * @file distric_obs.h
 * @brief DistriC Observability Library - High-performance metrics and logging
 * 
 * This library provides lock-free metrics collection and async structured logging
 * for distributed systems. All operations are thread-safe with minimal overhead.
 * 
 * Features:
 * - Lock-free metrics (counters, gauges, histograms)
 * - Prometheus-compatible export format
 * - Async JSON logging with ring buffer
 * - Zero external dependencies
 * - <1% CPU overhead
 * 
 * @version 0.1.0
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

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_OBS_H */