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

#define MAX_SPANS_BUFFER 1000

#define SPAN_SLOT_EMPTY      0
#define SPAN_SLOT_FILLED     1
#define SPAN_SLOT_PROCESSING 2

typedef struct {
    _Atomic uint32_t state;
    trace_span_t span;
} span_slot_t;

struct tracer_s {
    span_slot_t* slots;
    uint32_t buffer_mask;
    
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    
    trace_sampling_config_t sampling;
    _Atomic bool in_backpressure;
    _Atomic uint64_t sample_counter;
    
    void (*export_callback)(trace_span_t*, size_t, void*);
    void* user_data;
    
    _Atomic uint64_t spans_created;
    _Atomic uint64_t spans_sampled_in;
    _Atomic uint64_t spans_sampled_out;
    _Atomic uint64_t spans_dropped_backpressure;
    _Atomic uint64_t exports_attempted;
    _Atomic uint64_t exports_succeeded;
    
    _Atomic uint32_t refcount;
    _Atomic bool shutdown;
    pthread_t exporter_thread;
    bool exporter_started;
};

static __thread trace_span_t* tls_active_span = NULL;

static trace_span_t sampled_out_span = {
    .sampled = false
};

static uint32_t next_power_of_2(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static trace_id_t generate_trace_id(void) {
    trace_id_t id;
    id.high = ((uint64_t)rand() << 32) | rand();
    id.low = ((uint64_t)rand() << 32) | rand();
    return id;
}

static span_id_t generate_span_id(void) {
    return ((uint64_t)rand() << 32) | rand();
}

static uint32_t fast_rand(void) {
    static _Atomic uint32_t seed = 12345;
    uint32_t s = atomic_load_explicit(&seed, memory_order_relaxed);
    s = s * 1103515245 + 12345;
    atomic_store_explicit(&seed, s, memory_order_relaxed);
    return s;
}

static bool should_sample(tracer_t* tracer) {
    bool backpressure = atomic_load_explicit(&tracer->in_backpressure, memory_order_relaxed);
    
    uint32_t sample_rate, total_rate;
    if (backpressure) {
        sample_rate = tracer->sampling.backpressure_sample;
        total_rate = tracer->sampling.backpressure_sample +
                     tracer->sampling.backpressure_drop;
    } else {
        sample_rate = tracer->sampling.always_sample;
        total_rate = tracer->sampling.always_sample +
                     tracer->sampling.always_drop;
    }
    
    if (total_rate == 0) {
        return false;
    }
    
    uint32_t rand_val = fast_rand() % total_rate;
    return rand_val < sample_rate;
}

/*
 * Update the backpressure flag with hysteresis to prevent thrashing:
 *   Enter backpressure at 75% fill.
 *   Exit  backpressure at 50% fill.
 * Both thresholds use relaxed loads — approximate fill level is acceptable
 * for a sampling heuristic; exactness is not required.
 */
static void update_backpressure(tracer_t* tracer) {
    uint64_t head       = atomic_load_explicit(&tracer->head, memory_order_relaxed);
    uint64_t tail       = atomic_load_explicit(&tracer->tail, memory_order_relaxed);
    uint32_t buffer_size = tracer->buffer_mask + 1;
    uint64_t used       = (head >= tail) ? (head - tail) : 0;
    uint32_t fill_pct   = (uint32_t)(used * 100u / buffer_size);

    bool currently_in_bp = atomic_load_explicit(&tracer->in_backpressure,
                                                 memory_order_relaxed);
    bool should_be_in_bp;
    if (currently_in_bp) {
        should_be_in_bp = (fill_pct > 50); /* exit at 50% */
    } else {
        should_be_in_bp = (fill_pct > 75); /* enter at 75% */
    }

    if (should_be_in_bp != currently_in_bp) {
        /* Relaxed store — this is advisory sampling state. */
        atomic_store_explicit(&tracer->in_backpressure, should_be_in_bp,
                              memory_order_relaxed);
    }
}

static void* exporter_thread_fn(void* arg) {
    tracer_t* tracer = (tracer_t*)arg;
    
    while (!atomic_load_explicit(&tracer->shutdown, memory_order_acquire)) {
        uint64_t tail = atomic_load_explicit(&tracer->tail, memory_order_acquire);
        uint64_t head = atomic_load_explicit(&tracer->head, memory_order_acquire);
        
        if (tail == head) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
            nanosleep(&ts, NULL);
            update_backpressure(tracer);
            continue;
        }
        
        uint32_t slot_idx = tail & tracer->buffer_mask;
        span_slot_t* slot = &tracer->slots[slot_idx];
        
        uint32_t expected = SPAN_SLOT_FILLED;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, SPAN_SLOT_PROCESSING,
                memory_order_acquire, memory_order_relaxed)) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000};
            nanosleep(&ts, NULL);
            continue;
        }
        
        atomic_fetch_add_explicit(&tracer->exports_attempted, 1, memory_order_relaxed);
        
        if (tracer->export_callback) {
            trace_span_t spans[1];
            memcpy(&spans[0], &slot->span, sizeof(trace_span_t));
            tracer->export_callback(spans, 1, tracer->user_data);
            atomic_fetch_add_explicit(&tracer->exports_succeeded, 1, memory_order_relaxed);
        }
        
        atomic_store_explicit(&slot->state, SPAN_SLOT_EMPTY, memory_order_release);
        atomic_store_explicit(&tracer->tail, tail + 1, memory_order_release);
        
        update_backpressure(tracer);
    }
    
    return NULL;
}

distric_err_t trace_init(
    tracer_t** tracer,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data) {
    
    /* Default: 100% normal sampling, 10% under backpressure.
     * This means tracing volume automatically drops by 90% when the export
     * buffer exceeds 75% fill — graceful degradation under load. */
    trace_sampling_config_t default_sampling = {
        .always_sample        = 1,
        .always_drop          = 0,
        .backpressure_sample  = 1,
        .backpressure_drop    = 9,
    };
    
    return trace_init_with_sampling(tracer, &default_sampling, export_callback, user_data);
}

distric_err_t trace_init_with_sampling(
    tracer_t** tracer,
    const trace_sampling_config_t* sampling,
    void (*export_callback)(trace_span_t*, size_t, void*),
    void* user_data) {
    
    if (!tracer || !sampling) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tracer_t* t = calloc(1, sizeof(tracer_t));
    if (!t) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    t->sampling = *sampling;
    t->export_callback = export_callback;
    t->user_data = user_data;
    
    uint32_t buffer_size = next_power_of_2(MAX_SPANS_BUFFER);
    t->buffer_mask = buffer_size - 1;
    
    t->slots = calloc(buffer_size, sizeof(span_slot_t));
    if (!t->slots) {
        free(t);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    for (uint32_t i = 0; i < buffer_size; i++) {
        atomic_init(&t->slots[i].state, SPAN_SLOT_EMPTY);
    }
    
    atomic_init(&t->head, 0);
    atomic_init(&t->tail, 0);
    atomic_init(&t->spans_created, 0);
    atomic_init(&t->spans_sampled_in, 0);
    atomic_init(&t->spans_sampled_out, 0);
    atomic_init(&t->spans_dropped_backpressure, 0);
    atomic_init(&t->exports_attempted, 0);
    atomic_init(&t->exports_succeeded, 0);
    atomic_init(&t->sample_counter, 0);
    atomic_init(&t->in_backpressure, false);
    atomic_init(&t->refcount, 1);
    atomic_init(&t->shutdown, false);
    
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
    if (!tracer) return;
    atomic_fetch_add_explicit(&tracer->refcount, 1, memory_order_relaxed);
}

void trace_release(tracer_t* tracer) {
    if (!tracer) return;
    
    uint32_t old_ref = atomic_fetch_sub_explicit(&tracer->refcount, 1, memory_order_acq_rel);
    if (old_ref != 1) return;
    
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

distric_err_t trace_start_span(
    tracer_t* tracer,
    const char* operation,
    trace_span_t** out_span) {
    
    if (!tracer || !operation || !out_span) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    atomic_fetch_add_explicit(&tracer->spans_created, 1, memory_order_relaxed);
    
    if (!should_sample(tracer)) {
        atomic_fetch_add_explicit(&tracer->spans_sampled_out, 1, memory_order_relaxed);
        *out_span = &sampled_out_span;
        return DISTRIC_OK;
    }
    
    atomic_fetch_add_explicit(&tracer->spans_sampled_in, 1, memory_order_relaxed);
    
    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) {
        *out_span = &sampled_out_span;
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memset(span, 0, sizeof(*span));
    
    span->trace_id = generate_trace_id();
    span->span_id = generate_span_id();
    span->parent_span_id = 0;
    
    strncpy(span->operation, operation, TRACE_MAX_OPERATION_LEN - 1);
    span->operation[TRACE_MAX_OPERATION_LEN - 1] = '\0';
    
    span->start_time_ns = get_time_ns();
    span->tag_count = 0;
    span->status = SPAN_STATUS_UNSET;
    span->sampled = true;
    span->_tracer = tracer;
    
    *out_span = span;
    return DISTRIC_OK;
}

distric_err_t trace_start_child_span(
    tracer_t* tracer,
    trace_span_t* parent,
    const char* operation,
    trace_span_t** out_span) {
    
    if (!tracer || !parent || !operation || !out_span) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    atomic_fetch_add_explicit(&tracer->spans_created, 1, memory_order_relaxed);
    
    if (!should_sample(tracer)) {
        atomic_fetch_add_explicit(&tracer->spans_sampled_out, 1, memory_order_relaxed);
        *out_span = &sampled_out_span;
        return DISTRIC_OK;
    }
    
    atomic_fetch_add_explicit(&tracer->spans_sampled_in, 1, memory_order_relaxed);
    
    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) {
        *out_span = &sampled_out_span;
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memset(span, 0, sizeof(*span));
    
    span->trace_id = parent->trace_id;
    span->span_id = generate_span_id();
    span->parent_span_id = parent->span_id;
    
    strncpy(span->operation, operation, TRACE_MAX_OPERATION_LEN - 1);
    span->operation[TRACE_MAX_OPERATION_LEN - 1] = '\0';
    
    span->start_time_ns = get_time_ns();
    span->tag_count = 0;
    span->status = SPAN_STATUS_UNSET;
    span->sampled = true;
    span->_tracer = tracer;
    
    *out_span = span;
    return DISTRIC_OK;
}

distric_err_t trace_add_tag(trace_span_t* span, const char* key, const char* value) {
    if (!span || !key || !value || !span->sampled) {
        return DISTRIC_OK;
    }
    
    if (span->tag_count >= TRACE_MAX_SPAN_TAGS) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    
    span_tag_t* tag = &span->tags[span->tag_count];
    strncpy(tag->key, key, TRACE_MAX_TAG_KEY_LEN - 1);
    tag->key[TRACE_MAX_TAG_KEY_LEN - 1] = '\0';
    
    strncpy(tag->value, value, TRACE_MAX_TAG_VALUE_LEN - 1);
    tag->value[TRACE_MAX_TAG_VALUE_LEN - 1] = '\0';
    
    span->tag_count++;
    return DISTRIC_OK;
}

void trace_set_status(trace_span_t* span, span_status_t status) {
    if (span && span->sampled) {
        span->status = status;
    }
}

void trace_finish_span(tracer_t* tracer, trace_span_t* span) {
    if (!span || !span->sampled || span == &sampled_out_span) {
        return;
    }
    
    span->end_time_ns = get_time_ns();
    
    if (!tracer) {
        tracer = (struct tracer_s*)span->_tracer;
    }
    
    if (!tracer) {
        free(span);
        return;
    }
    
    uint64_t head = atomic_load_explicit(&tracer->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&tracer->tail, memory_order_acquire);
    uint32_t buffer_size = tracer->buffer_mask + 1;
    
    if (head - tail >= buffer_size) {
        atomic_fetch_add_explicit(&tracer->spans_dropped_backpressure, 1, memory_order_relaxed);
        free(span);
        return;
    }
    
    uint64_t slot_idx = atomic_fetch_add_explicit(&tracer->head, 1, memory_order_acq_rel);
    span_slot_t* slot = &tracer->slots[slot_idx & tracer->buffer_mask];
    
    memcpy(&slot->span, span, sizeof(trace_span_t));
    atomic_store_explicit(&slot->state, SPAN_SLOT_FILLED, memory_order_release);
    
    free(span);
}

distric_err_t trace_inject_context(trace_span_t* span, char* header, size_t header_size) {
    if (!span || !header || header_size < 128) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    int written = snprintf(header, header_size,
                          "00-%016lx%016lx-%016lx-01",
                          span->trace_id.high, span->trace_id.low,
                          span->span_id);
    
    if (written < 0 || (size_t)written >= header_size) {
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    
    return DISTRIC_OK;
}

distric_err_t trace_extract_context(const char* header, trace_context_t* context) {
    if (!header || !context) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
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

distric_err_t trace_start_span_from_context(
    tracer_t* tracer,
    const trace_context_t* context,
    const char* operation,
    trace_span_t** out_span) {
    
    if (!tracer || !context || !operation || !out_span) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    trace_span_t* span = malloc(sizeof(trace_span_t));
    if (!span) {
        *out_span = &sampled_out_span;
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memset(span, 0, sizeof(*span));
    
    span->trace_id = context->trace_id;
    span->span_id = generate_span_id();
    span->parent_span_id = context->span_id;
    
    strncpy(span->operation, operation, TRACE_MAX_OPERATION_LEN - 1);
    
    span->start_time_ns = get_time_ns();
    span->status = SPAN_STATUS_UNSET;
    span->sampled = true;
    span->_tracer = tracer;
    
    *out_span = span;
    return DISTRIC_OK;
}

void trace_set_active_span(trace_span_t* span) {
    tls_active_span = span;
}

trace_span_t* trace_get_active_span(void) {
    return tls_active_span;
}