#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>

const double HISTOGRAM_BUCKETS[10] = {
    0.001, 0.01, 0.1, 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, INFINITY
};

#define MAX_METRICS 1024
#define MAX_METRIC_NAME_LEN 128
#define MAX_METRIC_HELP_LEN 256
#define HISTOGRAM_BUCKET_COUNT 10

typedef enum {
    REGISTRY_STATE_MUTABLE = 0,
    REGISTRY_STATE_FROZEN = 1,
} registry_state_t;

typedef struct counter_instance_s {
    metric_label_t labels[MAX_METRIC_LABELS];
    uint32_t num_labels;
    _Atomic uint64_t value;
    struct counter_instance_s *next;
} counter_instance_t;

typedef struct gauge_instance_s {
    metric_label_t labels[MAX_METRIC_LABELS];
    uint32_t num_labels;
    _Atomic uint64_t value_bits;
    struct gauge_instance_s *next;
} gauge_instance_t;

typedef struct histogram_bucket_s {
    double upper_bound;
    _Atomic uint64_t count;
} histogram_bucket_t;

typedef struct histogram_instance_s {
    metric_label_t labels[MAX_METRIC_LABELS];
    uint32_t num_labels;
    histogram_bucket_t *buckets;
    uint32_t num_buckets;
    _Atomic uint64_t count;
    _Atomic uint64_t sum_bits;
    struct histogram_instance_s *next;
} histogram_instance_t;

struct metric_s {
    char name[MAX_METRIC_NAME_LEN];
    char help[MAX_METRIC_HELP_LEN];
    metric_type_t type;
    metric_label_definition_t label_defs[MAX_METRIC_LABELS];
    uint32_t num_label_defs;
    _Atomic bool initialized;
    
    union {
        struct {
            _Atomic(counter_instance_t *) instances;
            pthread_mutex_t instance_lock;
        } counter;
        struct {
            _Atomic(gauge_instance_t *) instances;
            pthread_mutex_t instance_lock;
        } gauge;
        struct {
            _Atomic(histogram_instance_t *) instances;
            pthread_mutex_t instance_lock;
            histogram_bucket_t buckets_template[HISTOGRAM_BUCKET_COUNT];
            uint32_t num_buckets;
        } histogram;
    } data;
};

struct metrics_registry_s {
    metric_t metrics[MAX_METRICS];
    _Atomic size_t metric_count;
    _Atomic uint32_t state;
    _Atomic uint32_t refcount;
    pthread_mutex_t register_mutex;
};

static bool validate_label(const metric_label_t *label,
                           const metric_label_definition_t *def) {
    if (strcmp(label->key, def->key) != 0) {
        return false;
    }
    
    if (!def->allowed_values || def->num_allowed_values == 0) {
        return true;
    }
    
    for (uint32_t i = 0; i < def->num_allowed_values; i++) {
        if (strcmp(label->value, def->allowed_values[i]) == 0) {
            return true;
        }
    }
    
    return false;
}

static distric_err_t validate_labels(const metric_label_t *labels,
                                      uint32_t num_labels,
                                      const metric_label_definition_t *defs,
                                      uint32_t num_defs) {
    if (num_labels != num_defs) {
        return DISTRIC_ERR_INVALID_LABEL;
    }
    
    for (uint32_t i = 0; i < num_labels; i++) {
        bool found = false;
        for (uint32_t j = 0; j < num_defs; j++) {
            if (validate_label(&labels[i], &defs[j])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return DISTRIC_ERR_INVALID_LABEL;
        }
    }
    
    return DISTRIC_OK;
}

static bool labels_equal(const metric_label_t *a, uint32_t a_count,
                        const metric_label_t *b, uint32_t b_count) {
    if (a_count != b_count) {
        return false;
    }
    
    for (uint32_t i = 0; i < a_count; i++) {
        if (strcmp(a[i].key, b[i].key) != 0 ||
            strcmp(a[i].value, b[i].value) != 0) {
            return false;
        }
    }
    
    return true;
}

distric_err_t metrics_init(metrics_registry_t** registry) {
    if (!registry) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    metrics_registry_t* reg = calloc(1, sizeof(metrics_registry_t));
    if (!reg) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    atomic_init(&reg->metric_count, 0);
    atomic_init(&reg->state, REGISTRY_STATE_MUTABLE);
    atomic_init(&reg->refcount, 1);
    pthread_mutex_init(&reg->register_mutex, NULL);
    
    for (size_t i = 0; i < MAX_METRICS; i++) {
        atomic_init(&reg->metrics[i].initialized, false);
    }
    
    *registry = reg;
    return DISTRIC_OK;
}

void metrics_destroy(metrics_registry_t* registry) {
    if (!registry) return;
    
    uint32_t old_ref = atomic_fetch_sub_explicit(&registry->refcount, 1, memory_order_acq_rel);
    if (old_ref != 1) return;
    
    pthread_mutex_destroy(&registry->register_mutex);
    free(registry);
}

distric_err_t metrics_freeze(metrics_registry_t* registry) {
    if (!registry) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    uint32_t expected = REGISTRY_STATE_MUTABLE;
    if (!atomic_compare_exchange_strong_explicit(
            &registry->state, &expected, REGISTRY_STATE_FROZEN,
            memory_order_release, memory_order_relaxed)) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }
    
    return DISTRIC_OK;
}

distric_err_t metrics_register_counter(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_definition_t* label_defs,
    size_t label_def_count,
    metric_t** out_metric) {
    
    if (!registry || !name || !out_metric) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (atomic_load_explicit(&registry->state, memory_order_acquire) == REGISTRY_STATE_FROZEN) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }
    
    pthread_mutex_lock(&registry->register_mutex);
    
    for (size_t i = 0; i < atomic_load(&registry->metric_count); i++) {
        if (strcmp(registry->metrics[i].name, name) == 0) {
            pthread_mutex_unlock(&registry->register_mutex);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }
    
    size_t idx = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    metric_t* metric = &registry->metrics[idx];
    
    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    metric->name[MAX_METRIC_NAME_LEN - 1] = '\0';
    
    if (help) {
        strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
        metric->help[MAX_METRIC_HELP_LEN - 1] = '\0';
    }
    
    metric->type = METRIC_TYPE_COUNTER;
    metric->num_label_defs = label_def_count > MAX_METRIC_LABELS ? MAX_METRIC_LABELS : label_def_count;
    
    if (label_defs && label_def_count > 0) {
        memcpy(metric->label_defs, label_defs, 
               metric->num_label_defs * sizeof(metric_label_definition_t));
    }
    
    atomic_init(&metric->data.counter.instances, NULL);
    pthread_mutex_init(&metric->data.counter.instance_lock, NULL);
    
    atomic_store_explicit(&metric->initialized, true, memory_order_release);
    atomic_store_explicit(&registry->metric_count, idx + 1, memory_order_release);
    
    pthread_mutex_unlock(&registry->register_mutex);
    
    *out_metric = metric;
    return DISTRIC_OK;
}

distric_err_t metrics_register_gauge(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_definition_t* label_defs,
    size_t label_def_count,
    metric_t** out_metric) {
    
    if (!registry || !name || !out_metric) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (atomic_load_explicit(&registry->state, memory_order_acquire) == REGISTRY_STATE_FROZEN) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }
    
    pthread_mutex_lock(&registry->register_mutex);
    
    for (size_t i = 0; i < atomic_load(&registry->metric_count); i++) {
        if (strcmp(registry->metrics[i].name, name) == 0) {
            pthread_mutex_unlock(&registry->register_mutex);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }
    
    size_t idx = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    metric_t* metric = &registry->metrics[idx];
    
    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    if (help) {
        strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
    }
    
    metric->type = METRIC_TYPE_GAUGE;
    metric->num_label_defs = label_def_count > MAX_METRIC_LABELS ? MAX_METRIC_LABELS : label_def_count;
    
    if (label_defs && label_def_count > 0) {
        memcpy(metric->label_defs, label_defs,
               metric->num_label_defs * sizeof(metric_label_definition_t));
    }
    
    atomic_init(&metric->data.gauge.instances, NULL);
    pthread_mutex_init(&metric->data.gauge.instance_lock, NULL);
    
    atomic_store_explicit(&metric->initialized, true, memory_order_release);
    atomic_store_explicit(&registry->metric_count, idx + 1, memory_order_release);
    
    pthread_mutex_unlock(&registry->register_mutex);
    
    *out_metric = metric;
    return DISTRIC_OK;
}

distric_err_t metrics_register_histogram(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_definition_t* label_defs,
    size_t label_def_count,
    metric_t** out_metric) {
    
    if (!registry || !name || !out_metric) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (atomic_load_explicit(&registry->state, memory_order_acquire) == REGISTRY_STATE_FROZEN) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }
    
    pthread_mutex_lock(&registry->register_mutex);
    
    for (size_t i = 0; i < atomic_load(&registry->metric_count); i++) {
        if (strcmp(registry->metrics[i].name, name) == 0) {
            pthread_mutex_unlock(&registry->register_mutex);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }
    
    size_t idx = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    metric_t* metric = &registry->metrics[idx];
    
    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    if (help) {
        strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
    }
    
    metric->type = METRIC_TYPE_HISTOGRAM;
    metric->num_label_defs = label_def_count > MAX_METRIC_LABELS ? MAX_METRIC_LABELS : label_def_count;
    
    if (label_defs && label_def_count > 0) {
        memcpy(metric->label_defs, label_defs,
               metric->num_label_defs * sizeof(metric_label_definition_t));
    }
    
    metric->data.histogram.num_buckets = HISTOGRAM_BUCKET_COUNT;
    for (uint32_t i = 0; i < HISTOGRAM_BUCKET_COUNT; i++) {
        metric->data.histogram.buckets_template[i].upper_bound = HISTOGRAM_BUCKETS[i];
        atomic_init(&metric->data.histogram.buckets_template[i].count, 0);
    }
    
    atomic_init(&metric->data.histogram.instances, NULL);
    pthread_mutex_init(&metric->data.histogram.instance_lock, NULL);
    
    atomic_store_explicit(&metric->initialized, true, memory_order_release);
    atomic_store_explicit(&registry->metric_count, idx + 1, memory_order_release);
    
    pthread_mutex_unlock(&registry->register_mutex);
    
    *out_metric = metric;
    return DISTRIC_OK;
}

distric_err_t metrics_counter_inc_with_labels(
    metric_t* metric,
    const metric_label_t* labels,
    size_t label_count,
    uint64_t value) {
    
    if (!metric || metric->type != METRIC_TYPE_COUNTER) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    distric_err_t err = validate_labels(labels, label_count,
                                        metric->label_defs, metric->num_label_defs);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    counter_instance_t* instance = atomic_load_explicit(&metric->data.counter.instances, memory_order_acquire);
    while (instance) {
        if (labels_equal(instance->labels, instance->num_labels, labels, label_count)) {
            atomic_fetch_add_explicit(&instance->value, value, memory_order_relaxed);
            return DISTRIC_OK;
        }
        instance = instance->next;
    }
    
    pthread_mutex_lock(&metric->data.counter.instance_lock);
    
    instance = atomic_load_explicit(&metric->data.counter.instances, memory_order_acquire);
    while (instance) {
        if (labels_equal(instance->labels, instance->num_labels, labels, label_count)) {
            pthread_mutex_unlock(&metric->data.counter.instance_lock);
            atomic_fetch_add_explicit(&instance->value, value, memory_order_relaxed);
            return DISTRIC_OK;
        }
        instance = instance->next;
    }
    
    counter_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&metric->data.counter.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(new_inst->labels, labels, label_count * sizeof(metric_label_t));
    new_inst->num_labels = label_count;
    atomic_init(&new_inst->value, value);
    
    new_inst->next = atomic_load_explicit(&metric->data.counter.instances, memory_order_relaxed);
    atomic_store_explicit(&metric->data.counter.instances, new_inst, memory_order_release);
    
    pthread_mutex_unlock(&metric->data.counter.instance_lock);
    return DISTRIC_OK;
}

distric_err_t metrics_gauge_set_with_labels(
    metric_t* metric,
    const metric_label_t* labels,
    size_t label_count,
    double value) {
    
    if (!metric || metric->type != METRIC_TYPE_GAUGE) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    distric_err_t err = validate_labels(labels, label_count,
                                        metric->label_defs, metric->num_label_defs);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    uint64_t value_bits;
    memcpy(&value_bits, &value, sizeof(uint64_t));
    
    gauge_instance_t* instance = atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    while (instance) {
        if (labels_equal(instance->labels, instance->num_labels, labels, label_count)) {
            atomic_store_explicit(&instance->value_bits, value_bits, memory_order_relaxed);
            return DISTRIC_OK;
        }
        instance = instance->next;
    }
    
    pthread_mutex_lock(&metric->data.gauge.instance_lock);
    
    instance = atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    while (instance) {
        if (labels_equal(instance->labels, instance->num_labels, labels, label_count)) {
            pthread_mutex_unlock(&metric->data.gauge.instance_lock);
            atomic_store_explicit(&instance->value_bits, value_bits, memory_order_relaxed);
            return DISTRIC_OK;
        }
        instance = instance->next;
    }
    
    gauge_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&metric->data.gauge.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(new_inst->labels, labels, label_count * sizeof(metric_label_t));
    new_inst->num_labels = label_count;
    atomic_init(&new_inst->value_bits, value_bits);
    
    new_inst->next = atomic_load_explicit(&metric->data.gauge.instances, memory_order_relaxed);
    atomic_store_explicit(&metric->data.gauge.instances, new_inst, memory_order_release);
    
    pthread_mutex_unlock(&metric->data.gauge.instance_lock);
    return DISTRIC_OK;
}

distric_err_t metrics_histogram_observe_with_labels(
    metric_t* metric,
    const metric_label_t* labels,
    size_t label_count,
    double value) {
    
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    distric_err_t err = validate_labels(labels, label_count,
                                        metric->label_defs, metric->num_label_defs);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    histogram_instance_t* instance = atomic_load_explicit(&metric->data.histogram.instances, memory_order_acquire);
    while (instance) {
        if (labels_equal(instance->labels, instance->num_labels, labels, label_count)) {
            atomic_fetch_add_explicit(&instance->count, 1, memory_order_relaxed);
            
            uint64_t sum_bits;
            memcpy(&sum_bits, &value, sizeof(uint64_t));
            atomic_fetch_add_explicit(&instance->sum_bits, sum_bits, memory_order_relaxed);
            
            for (uint32_t i = 0; i < instance->num_buckets; i++) {
                if (value <= instance->buckets[i].upper_bound) {
                    atomic_fetch_add_explicit(&instance->buckets[i].count, 1, memory_order_relaxed);
                    break;
                }
            }
            
            return DISTRIC_OK;
        }
        instance = instance->next;
    }
    
    pthread_mutex_lock(&metric->data.histogram.instance_lock);
    
    histogram_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&metric->data.histogram.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(new_inst->labels, labels, label_count * sizeof(metric_label_t));
    new_inst->num_labels = label_count;
    
    new_inst->num_buckets = metric->data.histogram.num_buckets;
    new_inst->buckets = calloc(new_inst->num_buckets, sizeof(histogram_bucket_t));
    if (!new_inst->buckets) {
        free(new_inst);
        pthread_mutex_unlock(&metric->data.histogram.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    for (uint32_t i = 0; i < new_inst->num_buckets; i++) {
        new_inst->buckets[i].upper_bound = metric->data.histogram.buckets_template[i].upper_bound;
        atomic_init(&new_inst->buckets[i].count, 0);
        if (value <= new_inst->buckets[i].upper_bound) {
            atomic_store_explicit(&new_inst->buckets[i].count, 1, memory_order_relaxed);
        }
    }
    
    atomic_init(&new_inst->count, 1);
    uint64_t sum_bits;
    memcpy(&sum_bits, &value, sizeof(uint64_t));
    atomic_init(&new_inst->sum_bits, sum_bits);
    
    new_inst->next = atomic_load_explicit(&metric->data.histogram.instances, memory_order_relaxed);
    atomic_store_explicit(&metric->data.histogram.instances, new_inst, memory_order_release);
    
    pthread_mutex_unlock(&metric->data.histogram.instance_lock);
    return DISTRIC_OK;
}

void metrics_counter_inc(metric_t* metric) {
    metrics_counter_inc_with_labels(metric, NULL, 0, 1);
}

void metrics_counter_add(metric_t* metric, uint64_t value) {
    metrics_counter_inc_with_labels(metric, NULL, 0, value);
}

void metrics_gauge_set(metric_t* metric, double value) {
    metrics_gauge_set_with_labels(metric, NULL, 0, value);
}

void metrics_histogram_observe(metric_t* metric, double value) {
    metrics_histogram_observe_with_labels(metric, NULL, 0, value);
}

distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char** out_buffer,
    size_t* out_size) {
    
    if (!registry || !out_buffer || !out_size) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t buffer_size = 65536;
    char* buffer = malloc(buffer_size);
    if (!buffer) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    size_t offset = 0;
    size_t count = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    
    for (size_t i = 0; i < count; i++) {
        metric_t* m = &registry->metrics[i];
        
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) {
            continue;
        }
        
        int written = snprintf(buffer + offset, buffer_size - offset,
                              "# HELP %s %s\n# TYPE %s ",
                              m->name, m->help,
                              m->name);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        
        const char* type_str = "untyped";
        if (m->type == METRIC_TYPE_COUNTER) type_str = "counter";
        else if (m->type == METRIC_TYPE_GAUGE) type_str = "gauge";
        else if (m->type == METRIC_TYPE_HISTOGRAM) type_str = "histogram";
        
        written = snprintf(buffer + offset, buffer_size - offset, "%s\n", type_str);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        
        if (m->type == METRIC_TYPE_COUNTER) {
            counter_instance_t* inst = atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
            while (inst) {
                uint64_t value = atomic_load_explicit(&inst->value, memory_order_relaxed);
                written = snprintf(buffer + offset, buffer_size - offset,
                                 "%s %lu\n", m->name, value);
                if (written > 0) offset += written;
                inst = inst->next;
            }
        } else if (m->type == METRIC_TYPE_GAUGE) {
            gauge_instance_t* inst = atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
            while (inst) {
                uint64_t bits = atomic_load_explicit(&inst->value_bits, memory_order_relaxed);
                double value;
                memcpy(&value, &bits, sizeof(double));
                written = snprintf(buffer + offset, buffer_size - offset,
                                 "%s %.6f\n", m->name, value);
                if (written > 0) offset += written;
                inst = inst->next;
            }
        } else if (m->type == METRIC_TYPE_HISTOGRAM) {
            histogram_instance_t* inst = atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
            while (inst) {
                for (uint32_t b = 0; b < inst->num_buckets; b++) {
                    uint64_t bucket_count = atomic_load_explicit(&inst->buckets[b].count, memory_order_relaxed);
                    if (isinf(inst->buckets[b].upper_bound)) {
                        written = snprintf(buffer + offset, buffer_size - offset,
                                         "%s_bucket{le=\"+Inf\"} %lu\n", m->name, bucket_count);
                    } else {
                        written = snprintf(buffer + offset, buffer_size - offset,
                                         "%s_bucket{le=\"%.3f\"} %lu\n",
                                         m->name, inst->buckets[b].upper_bound, bucket_count);
                    }
                    if (written > 0) offset += written;
                }
                
                uint64_t sum_bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
                double sum;
                memcpy(&sum, &sum_bits, sizeof(double));
                uint64_t hist_count = atomic_load_explicit(&inst->count, memory_order_relaxed);
                
                written = snprintf(buffer + offset, buffer_size - offset,
                                 "%s_sum %.6f\n%s_count %lu\n",
                                 m->name, sum, m->name, hist_count);
                if (written > 0) offset += written;
                
                inst = inst->next;
            }
        }
    }
    
    *out_buffer = buffer;
    *out_size = offset;
    
    return DISTRIC_OK;
}

uint64_t metrics_counter_get(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_COUNTER) return 0;
    counter_instance_t* inst = atomic_load_explicit(&metric->data.counter.instances, memory_order_acquire);
    uint64_t total = 0;
    while (inst) {
        total += atomic_load_explicit(&inst->value, memory_order_relaxed);
        inst = inst->next;
    }
    return total;
}

double metrics_gauge_get(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_GAUGE) return 0.0;
    gauge_instance_t* inst = atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    if (!inst) return 0.0;
    uint64_t bits = atomic_load_explicit(&inst->value_bits, memory_order_relaxed);
    double value; memcpy(&value, &bits, sizeof(double));
    return value;
}

uint64_t metrics_histogram_get_count(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) return 0;
    histogram_instance_t* inst = atomic_load_explicit(&metric->data.histogram.instances, memory_order_acquire);
    uint64_t total = 0;
    while (inst) {
        total += atomic_load_explicit(&inst->count, memory_order_relaxed);
        inst = inst->next;
    }
    return total;
}

double metrics_histogram_get_sum(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) return 0.0;
    histogram_instance_t* inst = atomic_load_explicit(&metric->data.histogram.instances, memory_order_acquire);
    if (!inst) return 0.0;
    uint64_t bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
    double value; memcpy(&value, &bits, sizeof(double));
    return value;
}