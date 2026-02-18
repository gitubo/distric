/**
 * @file metrics_internal.h
 * @brief DistriC Observability — Metrics Internal Implementation Details
 *
 * NOT part of the public API.  Must not be included by external consumers.
 * Only metrics.c and test_failure_modes.c (via whitelist) may include this.
 */

#ifndef DISTRIC_METRICS_INTERNAL_H
#define DISTRIC_METRICS_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>

/* ============================================================================
 * Compile-time internal defaults (NOT exported as public constants)
 * ========================================================================= */

/* Alias public hard-caps to internal names for readability */
#define MAX_METRICS            DISTRIC_MAX_METRICS
#define MAX_METRIC_LABELS      DISTRIC_MAX_METRIC_LABELS
#define MAX_METRIC_NAME_LEN    DISTRIC_MAX_METRIC_NAME_LEN
#define MAX_METRIC_HELP_LEN    DISTRIC_MAX_METRIC_HELP_LEN
#define MAX_LABEL_KEY_LEN      DISTRIC_MAX_LABEL_KEY_LEN
#define MAX_LABEL_VALUE_LEN    DISTRIC_MAX_LABEL_VALUE_LEN
#define MAX_METRIC_CARDINALITY DISTRIC_MAX_METRIC_CARDINALITY

#define HISTOGRAM_BUCKET_COUNT 10

/* ============================================================================
 * Registry state machine
 * ========================================================================= */

typedef enum {
    REGISTRY_STATE_MUTABLE = 0,
    REGISTRY_STATE_FROZEN  = 1,
    REGISTRY_STATE_DESTROYED = 2,
} registry_state_t;

/* ============================================================================
 * Internal instance types — lock-free linked lists per metric
 * ========================================================================= */

typedef struct counter_instance_s {
    metric_label_t              labels[MAX_METRIC_LABELS];
    uint32_t                    num_labels;
    _Atomic uint64_t            value;
    struct counter_instance_s*  next;
} counter_instance_t;

typedef struct gauge_instance_s {
    metric_label_t              labels[MAX_METRIC_LABELS];
    uint32_t                    num_labels;
    _Atomic uint64_t            value_bits; /* double stored as uint64 */
    struct gauge_instance_s*    next;
} gauge_instance_t;

typedef struct histogram_bucket_s {
    double           upper_bound;
    _Atomic uint64_t count;
} histogram_bucket_t;

typedef struct histogram_instance_s {
    metric_label_t               labels[MAX_METRIC_LABELS];
    uint32_t                     num_labels;
    histogram_bucket_t*          buckets;
    uint32_t                     num_buckets;
    _Atomic uint64_t             count;
    _Atomic uint64_t             sum_bits; /* double stored as uint64 */
    struct histogram_instance_s* next;
} histogram_instance_t;

/* ============================================================================
 * Internal metric_t layout
 * ========================================================================= */

struct metric_s {
    char              name[MAX_METRIC_NAME_LEN];
    char              help[MAX_METRIC_HELP_LEN];
    metric_type_t     type;
    metric_label_definition_t label_defs[MAX_METRIC_LABELS];
    uint32_t          num_label_defs;
    _Atomic bool      initialized;

    union {
        struct {
            _Atomic(counter_instance_t*)   instances;
            pthread_mutex_t                instance_lock;
        } counter;
        struct {
            _Atomic(gauge_instance_t*)     instances;
            pthread_mutex_t                instance_lock;
        } gauge;
        struct {
            _Atomic(histogram_instance_t*) instances;
            pthread_mutex_t                instance_lock;
            double                         buckets_template[HISTOGRAM_BUCKET_COUNT];
            uint32_t                       num_buckets;
        } histogram;
    } data;
};

/* ============================================================================
 * Internal registry layout
 * ========================================================================= */

struct metrics_registry_s {
    metric_t         metrics[MAX_METRICS];
    size_t           effective_max;        /* ≤ MAX_METRICS; from config */
    size_t           effective_cardinality_cap;
    _Atomic size_t   metric_count;
    _Atomic uint32_t state;                /* registry_state_t */
    _Atomic uint32_t refcount;
    pthread_mutex_t  register_mutex;
};

/* ============================================================================
 * Internal backpressure metrics handles (registered via metrics_register_*)
 * Updated by metrics_register_internal_metrics() in metrics.c
 * ========================================================================= */

/* (Currently no internal metrics for the registry itself; may be added later.) */

/* ============================================================================
 * Lifecycle assertion helpers
 * ========================================================================= */

#ifdef NDEBUG
#define METRICS_ASSERT_LIFECYCLE(cond) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define METRICS_ASSERT_LIFECYCLE(cond)                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[distric_obs] LIFECYCLE VIOLATION: %s "         \
                    "(%s:%d)\n", #cond, __FILE__, __LINE__);                  \
            abort();                                                           \
        }                                                                     \
    } while (0)
#endif

#endif /* DISTRIC_METRICS_INTERNAL_H */