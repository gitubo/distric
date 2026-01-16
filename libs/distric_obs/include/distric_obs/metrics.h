#ifndef DISTRIC_OBS_METRICS_H
#define DISTRIC_OBS_METRICS_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>

/* Internal metrics structures */

#define MAX_METRICS 1024
#define MAX_METRIC_NAME_LEN 128
#define MAX_METRIC_HELP_LEN 256
#define HISTOGRAM_BUCKET_COUNT 10

/* Fixed histogram buckets: 0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, +Inf */
extern const double HISTOGRAM_BUCKETS[HISTOGRAM_BUCKET_COUNT];

typedef struct {
    _Atomic uint64_t value;
} counter_data_t;

typedef struct {
    _Atomic uint64_t bits;  /* double stored as uint64_t for atomic ops */
} gauge_data_t;

typedef struct {
    _Atomic uint64_t buckets[HISTOGRAM_BUCKET_COUNT];
    _Atomic uint64_t count;
    _Atomic uint64_t sum_bits;  /* sum stored as uint64_t */
} histogram_data_t;

struct metric_s {
    char name[MAX_METRIC_NAME_LEN];
    char help[MAX_METRIC_HELP_LEN];
    metric_type_t type;
    metric_label_t labels[MAX_METRIC_LABELS];
    size_t label_count;
    _Atomic bool initialized;  /* Protects against reading partially initialized metrics */
    
    union {
        counter_data_t counter;
        gauge_data_t gauge;
        histogram_data_t histogram;
    } data;
};

struct metrics_registry_s {
    metric_t metrics[MAX_METRICS];
    _Atomic size_t metric_count;
    pthread_mutex_t register_mutex;  /* Protects registration for deduplication */
};

#endif /* DISTRIC_OBS_METRICS_H */