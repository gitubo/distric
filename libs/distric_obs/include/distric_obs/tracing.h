/**
 * @file tracing.h
 * @brief Internal tracing implementation details
 * 
 * This header contains implementation-specific structures.
 * Public API is in distric_obs.h
 */

#ifndef DISTRIC_OBS_TRACING_H
#define DISTRIC_OBS_TRACING_H

#include "distric_obs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/* Maximum limits */
#define MAX_SPAN_TAGS 16
#define MAX_TAG_KEY_LEN 64
#define MAX_TAG_VALUE_LEN 256
#define MAX_OPERATION_NAME_LEN 128
#define MAX_SPANS_BUFFER 1000
#define SPAN_EXPORT_INTERVAL_MS 5000

/* Span tag - internal structure */
typedef struct {
    char key[MAX_TAG_KEY_LEN];
    char value[MAX_TAG_VALUE_LEN];
} span_tag_t;

/* Trace span structure - internal implementation */
struct trace_span_s {
    trace_id_t trace_id;
    span_id_t span_id;
    span_id_t parent_span_id;
    char operation[MAX_OPERATION_NAME_LEN];
    
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    
    span_tag_t tags[MAX_SPAN_TAGS];
    size_t tag_count;
    
    span_status_t status;
    _Atomic bool finished;
};

/* Span buffer for batching - internal */
typedef struct {
    trace_span_t spans[MAX_SPANS_BUFFER];
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    _Atomic bool running;
    pthread_mutex_t lock;
} span_buffer_t;

/* Tracer registry - internal implementation */
struct tracer_s {
    span_buffer_t* buffer;
    pthread_t export_thread;
    _Atomic bool shutdown;
    
    /* Export callback - user provides this */
    void (*export_callback)(trace_span_t* spans, size_t count, void* user_data);
    void* user_data;
};

#endif /* DISTRIC_OBS_TRACING_H */