#ifndef DISTRIC_OBS_TRACING_H
#define DISTRIC_OBS_TRACING_H

#include "distric_obs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

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

/* Maximum limits */
#define MAX_SPAN_TAGS 16
#define MAX_TAG_KEY_LEN 64
#define MAX_TAG_VALUE_LEN 256
#define MAX_OPERATION_NAME_LEN 128
#define MAX_SPANS_BUFFER 1000
#define SPAN_EXPORT_INTERVAL_MS 5000

/* Span tag */
typedef struct {
    char key[MAX_TAG_KEY_LEN];
    char value[MAX_TAG_VALUE_LEN];
} span_tag_t;

/* Trace span structure */
typedef struct trace_span_s {
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
} trace_span_t;

/* Trace context for propagation */
typedef struct {
    trace_id_t trace_id;
    span_id_t span_id;
} trace_context_t;

/* Span buffer for batching */
typedef struct {
    trace_span_t spans[MAX_SPANS_BUFFER];
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    _Atomic bool running;
    pthread_mutex_t lock;
} span_buffer_t;

/* Tracer registry */
typedef struct tracer_s {
    span_buffer_t* buffer;
    pthread_t export_thread;
    _Atomic bool shutdown;
    
    /* Export callback - user provides this */
    void (*export_callback)(trace_span_t* spans, size_t count, void* user_data);
    void* user_data;
} tracer_t;

/* Initialize tracer with export callback */
distric_err_t trace_init(tracer_t** tracer, 
                         void (*export_callback)(trace_span_t*, size_t, void*),
                         void* user_data);

/* Destroy tracer and flush pending spans */
void trace_destroy(tracer_t* tracer);

/* Start a new root span */
distric_err_t trace_start_span(tracer_t* tracer, const char* operation, 
                               trace_span_t** out_span);

/* Start a child span */
distric_err_t trace_start_child_span(tracer_t* tracer, trace_span_t* parent,
                                     const char* operation, trace_span_t** out_span);

/* Add tag to span */
distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value);

/* Set span status */
void trace_set_status(trace_span_t* span, span_status_t status);

/* Finish span and submit for export */
void trace_finish_span(tracer_t* tracer, trace_span_t* span);

/* Context propagation - inject trace context into header string */
distric_err_t trace_inject_context(trace_span_t* span, char* header, size_t header_size);

/* Context propagation - extract trace context from header string */
distric_err_t trace_extract_context(const char* header, trace_context_t* context);

/* Create child span from extracted context */
distric_err_t trace_start_span_from_context(tracer_t* tracer, 
                                            const trace_context_t* context,
                                            const char* operation,
                                            trace_span_t** out_span);

/* Thread-local active span support */
void trace_set_active_span(trace_span_t* span);
trace_span_t* trace_get_active_span(void);

#endif /* DISTRIC_OBS_TRACING_H */