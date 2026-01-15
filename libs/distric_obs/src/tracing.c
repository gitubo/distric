#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "distric_obs/tracing.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* Thread-local active span */
static __thread trace_span_t* tls_active_span = NULL;

/* Get high-resolution timestamp in nanoseconds */
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Generate random trace ID */
static trace_id_t generate_trace_id(void) {
    trace_id_t id;
    /* Simple random generation - in production use better entropy source */
    id.high = ((uint64_t)rand() << 32) | rand();
    id.low = ((uint64_t)rand() << 32) | rand();
    return id;
}

/* Generate random span ID */
static span_id_t generate_span_id(void) {
    return ((uint64_t)rand() << 32) | rand();
}

/* Background export thread */
static void* export_thread_fn(void* arg) {
    tracer_t* tracer = (tracer_t*)arg;
    span_buffer_t* buffer = tracer->buffer;
    
    struct timespec sleep_time = {
        .tv_sec = SPAN_EXPORT_INTERVAL_MS / 1000,
        .tv_nsec = (SPAN_EXPORT_INTERVAL_MS % 1000) * 1000000
    };
    
    while (atomic_load(&buffer->running)) {
        nanosleep(&sleep_time, NULL);
        
        pthread_mutex_lock(&buffer->lock);
        
        size_t read_pos = atomic_load(&buffer->read_pos);
        size_t write_pos = atomic_load(&buffer->write_pos);
        size_t available = write_pos - read_pos;
        
        if (available > 0 && tracer->export_callback) {
            /* Export up to available spans */
            size_t to_export = available > MAX_SPANS_BUFFER ? MAX_SPANS_BUFFER : available;
            
            /* Create temporary array for export */
            trace_span_t* export_spans = malloc(to_export * sizeof(trace_span_t));
            if (export_spans) {
                for (size_t i = 0; i < to_export; i++) {
                    size_t idx = (read_pos + i) % MAX_SPANS_BUFFER;
                    memcpy(&export_spans[i], &buffer->spans[idx], sizeof(trace_span_t));
                }
                
                /* Call user callback */
                tracer->export_callback(export_spans, to_export, tracer->user_data);
                
                free(export_spans);
                
                /* Update read position */
                atomic_store(&buffer->read_pos, read_pos + to_export);
            }
        }
        
        pthread_mutex_unlock(&buffer->lock);
    }
    
    /* Final flush on shutdown */
    pthread_mutex_lock(&buffer->lock);
    size_t read_pos = atomic_load(&buffer->read_pos);
    size_t write_pos = atomic_load(&buffer->write_pos);
    size_t remaining = write_pos - read_pos;
    
    if (remaining > 0 && tracer->export_callback) {
        trace_span_t* export_spans = malloc(remaining * sizeof(trace_span_t));
        if (export_spans) {
            for (size_t i = 0; i < remaining; i++) {
                size_t idx = (read_pos + i) % MAX_SPANS_BUFFER;
                memcpy(&export_spans[i], &buffer->spans[idx], sizeof(trace_span_t));
            }
            
            tracer->export_callback(export_spans, remaining, tracer->user_data);
            free(export_spans);
        }
    }
    pthread_mutex_unlock(&buffer->lock);
    
    return NULL;
}

/* Initialize tracer */
distric_err_t trace_init(tracer_t** tracer,
                         void (*export_callback)(trace_span_t*, size_t, void*),
                         void* user_data) {
    if (!tracer) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tracer_t* t = calloc(1, sizeof(tracer_t));
    if (!t) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    t->buffer = calloc(1, sizeof(span_buffer_t));
    if (!t->buffer) {
        free(t);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    atomic_init(&t->buffer->write_pos, 0);
    atomic_init(&t->buffer->read_pos, 0);
    atomic_init(&t->buffer->running, true);
    
    if (pthread_mutex_init(&t->buffer->lock, NULL) != 0) {
        free(t->buffer);
        free(t);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    atomic_init(&t->shutdown, false);
    t->export_callback = export_callback;
    t->user_data = user_data;
    
    /* Start export thread */
    if (pthread_create(&t->export_thread, NULL, export_thread_fn, t) != 0) {
        pthread_mutex_destroy(&t->buffer->lock);
        free(t->buffer);
        free(t);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    *tracer = t;
    return DISTRIC_OK;
}

/* Destroy tracer */
void trace_destroy(tracer_t* tracer) {
    if (!tracer) {
        return;
    }
    
    if (tracer->buffer) {
        atomic_store(&tracer->buffer->running, false);
        pthread_join(tracer->export_thread, NULL);
        pthread_mutex_destroy(&tracer->buffer->lock);
        free(tracer->buffer);
    }
    
    free(tracer);
}

/* Start root span */
distric_err_t trace_start_span(tracer_t* tracer, const char* operation,
                               trace_span_t** out_span) {
    if (!tracer || !operation || !out_span) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    trace_span_t* span = calloc(1, sizeof(trace_span_t));
    if (!span) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    span->trace_id = generate_trace_id();
    span->span_id = generate_span_id();
    span->parent_span_id = 0;
    
    strncpy(span->operation, operation, MAX_OPERATION_NAME_LEN - 1);
    span->operation[MAX_OPERATION_NAME_LEN - 1] = '\0';
    
    span->start_time_ns = get_time_ns();
    span->end_time_ns = 0;
    span->tag_count = 0;
    span->status = SPAN_STATUS_UNSET;
    atomic_init(&span->finished, false);
    
    *out_span = span;
    return DISTRIC_OK;
}

/* Start child span */
distric_err_t trace_start_child_span(tracer_t* tracer, trace_span_t* parent,
                                     const char* operation, trace_span_t** out_span) {
    if (!tracer || !parent || !operation || !out_span) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    trace_span_t* span = calloc(1, sizeof(trace_span_t));
    if (!span) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    span->trace_id = parent->trace_id;
    span->span_id = generate_span_id();
    span->parent_span_id = parent->span_id;
    
    strncpy(span->operation, operation, MAX_OPERATION_NAME_LEN - 1);
    span->operation[MAX_OPERATION_NAME_LEN - 1] = '\0';
    
    span->start_time_ns = get_time_ns();
    span->end_time_ns = 0;
    span->tag_count = 0;
    span->status = SPAN_STATUS_UNSET;
    atomic_init(&span->finished, false);
    
    *out_span = span;
    return DISTRIC_OK;
}

/* Add tag to span */
distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value) {
    if (!span || !key || !value) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (span->tag_count >= MAX_SPAN_TAGS) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    
    span_tag_t* tag = &span->tags[span->tag_count];
    strncpy(tag->key, key, MAX_TAG_KEY_LEN - 1);
    tag->key[MAX_TAG_KEY_LEN - 1] = '\0';
    
    strncpy(tag->value, value, MAX_TAG_VALUE_LEN - 1);
    tag->value[MAX_TAG_VALUE_LEN - 1] = '\0';
    
    span->tag_count++;
    return DISTRIC_OK;
}

/* Set span status */
void trace_set_status(trace_span_t* span, span_status_t status) {
    if (span) {
        span->status = status;
    }
}

/* Finish span */
void trace_finish_span(tracer_t* tracer, trace_span_t* span) {
    if (!tracer || !span) {
        return;
    }
    
    /* Mark span as finished */
    span->end_time_ns = get_time_ns();
    atomic_store(&span->finished, true);
    
    /* Add to buffer */
    pthread_mutex_lock(&tracer->buffer->lock);
    
    size_t write_pos = atomic_load(&tracer->buffer->write_pos);
    size_t read_pos = atomic_load(&tracer->buffer->read_pos);
    
    /* Check if buffer is full */
    if (write_pos - read_pos >= MAX_SPANS_BUFFER) {
        pthread_mutex_unlock(&tracer->buffer->lock);
        free(span);
        return;
    }
    
    /* Copy span to buffer */
    size_t idx = write_pos % MAX_SPANS_BUFFER;
    memcpy(&tracer->buffer->spans[idx], span, sizeof(trace_span_t));
    
    atomic_store(&tracer->buffer->write_pos, write_pos + 1);
    
    pthread_mutex_unlock(&tracer->buffer->lock);
    
    /* Free the span memory */
    free(span);
}

/* Inject trace context into header */
distric_err_t trace_inject_context(trace_span_t* span, char* header, size_t header_size) {
    if (!span || !header || header_size < 128) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Format: traceparent: 00-<trace_id>-<span_id>-01 */
    int written = snprintf(header, header_size,
                          "00-%016lx%016lx-%016lx-01",
                          span->trace_id.high, span->trace_id.low,
                          span->span_id);
    
    if (written < 0 || (size_t)written >= header_size) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    
    return DISTRIC_OK;
}

/* Extract trace context from header */
distric_err_t trace_extract_context(const char* header, trace_context_t* context) {
    if (!header || !context) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Parse format: 00-<trace_id>-<span_id>-01 */
    unsigned long long trace_high, trace_low, span_id;
    int parsed = sscanf(header, "00-%016llx%016llx-%016llx-",
                       &trace_high, &trace_low, &span_id);
    
    if (parsed != 3) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    context->trace_id.high = trace_high;
    context->trace_id.low = trace_low;
    context->span_id = span_id;
    
    return DISTRIC_OK;
}

/* Start span from extracted context */
distric_err_t trace_start_span_from_context(tracer_t* tracer,
                                            const trace_context_t* context,
                                            const char* operation,
                                            trace_span_t** out_span) {
    if (!tracer || !context || !operation || !out_span) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    trace_span_t* span = calloc(1, sizeof(trace_span_t));
    if (!span) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    span->trace_id = context->trace_id;
    span->span_id = generate_span_id();
    span->parent_span_id = context->span_id;
    
    strncpy(span->operation, operation, MAX_OPERATION_NAME_LEN - 1);
    span->operation[MAX_OPERATION_NAME_LEN - 1] = '\0';
    
    span->start_time_ns = get_time_ns();
    span->end_time_ns = 0;
    span->tag_count = 0;
    span->status = SPAN_STATUS_UNSET;
    atomic_init(&span->finished, false);
    
    *out_span = span;
    return DISTRIC_OK;
}

/* Set thread-local active span */
void trace_set_active_span(trace_span_t* span) {
    tls_active_span = span;
}

/* Get thread-local active span */
trace_span_t* trace_get_active_span(void) {
    return tls_active_span;
}