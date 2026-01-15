#include "distric_obs/metrics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

const double HISTOGRAM_BUCKETS[HISTOGRAM_BUCKET_COUNT] = {
    0.001, 0.01, 0.1, 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, INFINITY
};

/* Helper: Convert double to uint64_t for atomic storage */
static inline uint64_t double_to_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(double));
    return bits;
}

/* Helper: Convert uint64_t back to double */
static inline double bits_to_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(double));
    return value;
}

/* Initialize a new metrics registry */
distric_err_t metrics_init(metrics_registry_t** registry) {
    if (!registry) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    metrics_registry_t* reg = calloc(1, sizeof(metrics_registry_t));
    if (!reg) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    atomic_init(&reg->metric_count, 0);
    
    /* FIX #4: Initialize mutex for registration deduplication */
    if (pthread_mutex_init(&reg->register_mutex, NULL) != 0) {
        free(reg);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Initialize all metrics as not initialized */
    for (size_t i = 0; i < MAX_METRICS; i++) {
        atomic_init(&reg->metrics[i].initialized, false);
    }
    
    *registry = reg;
    
    return DISTRIC_OK;
}

/* Destroy metrics registry and free all resources */
void metrics_destroy(metrics_registry_t* registry) {
    if (registry) {
        pthread_mutex_destroy(&registry->register_mutex);
        free(registry);
    }
}

/* Helper: Check if metric with same name and labels exists */
static metric_t* find_existing_metric(
    metrics_registry_t* registry,
    const char* name,
    const metric_label_t* labels,
    size_t label_count
) {
    size_t count = atomic_load(&registry->metric_count);
    
    for (size_t i = 0; i < count; i++) {
        metric_t* m = &registry->metrics[i];
        
        /* FIX #3: Only check initialized metrics */
        if (!atomic_load(&m->initialized)) {
            continue;
        }
        
        /* Check name match */
        if (strcmp(m->name, name) != 0) {
            continue;
        }
        
        /* Check label count match */
        if (m->label_count != label_count) {
            continue;
        }
        
        /* Check all labels match */
        bool labels_match = true;
        for (size_t j = 0; j < label_count; j++) {
            if (strcmp(m->labels[j].key, labels[j].key) != 0 ||
                strcmp(m->labels[j].value, labels[j].value) != 0) {
                labels_match = false;
                break;
            }
        }
        
        if (labels_match) {
            return m;
        }
    }
    
    return NULL;
}

/* Helper: Register a metric with common logic */
static distric_err_t register_metric(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    metric_type_t type,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
) {
    if (!registry || !name || !help || !out_metric) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (label_count > MAX_METRIC_LABELS) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* FIX #4: Lock for deduplication check and registration */
    pthread_mutex_lock(&registry->register_mutex);
    
    /* Check if metric already exists */
    metric_t* existing = find_existing_metric(registry, name, labels, label_count);
    if (existing) {
        pthread_mutex_unlock(&registry->register_mutex);
        
        /* Return existing metric if type matches */
        if (existing->type == type) {
            *out_metric = existing;
            return DISTRIC_OK;
        } else {
            /* Type mismatch - error */
            return DISTRIC_ERR_INVALID_ARG;
        }
    }
    
    /* Allocate new slot */
    size_t idx = atomic_fetch_add(&registry->metric_count, 1);
    if (idx >= MAX_METRICS) {
        atomic_fetch_sub(&registry->metric_count, 1);
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    metric_t* metric = &registry->metrics[idx];
    
    /* FIX #3: Mark as not initialized during setup */
    atomic_store(&metric->initialized, false);
    
    /* Copy metadata */
    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    metric->name[MAX_METRIC_NAME_LEN - 1] = '\0';
    
    strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
    metric->help[MAX_METRIC_HELP_LEN - 1] = '\0';
    
    metric->type = type;
    metric->label_count = label_count;
    
    for (size_t i = 0; i < label_count; i++) {
        strncpy(metric->labels[i].key, labels[i].key, MAX_LABEL_KEY_LEN - 1);
        metric->labels[i].key[MAX_LABEL_KEY_LEN - 1] = '\0';
        
        strncpy(metric->labels[i].value, labels[i].value, MAX_LABEL_VALUE_LEN - 1);
        metric->labels[i].value[MAX_LABEL_VALUE_LEN - 1] = '\0';
    }
    
    /* Initialize metric data based on type */
    switch (type) {
        case METRIC_TYPE_COUNTER:
            atomic_init(&metric->data.counter.value, 0);
            break;
        case METRIC_TYPE_GAUGE:
            atomic_init(&metric->data.gauge.bits, double_to_bits(0.0));
            break;
        case METRIC_TYPE_HISTOGRAM:
            atomic_init(&metric->data.histogram.count, 0);
            atomic_init(&metric->data.histogram.sum_bits, double_to_bits(0.0));
            for (int i = 0; i < HISTOGRAM_BUCKET_COUNT; i++) {
                atomic_init(&metric->data.histogram.buckets[i], 0);
            }
            break;
    }
    
    /* FIX #3: Mark as initialized - must be last step */
    atomic_store(&metric->initialized, true);
    
    pthread_mutex_unlock(&registry->register_mutex);
    
    *out_metric = metric;
    return DISTRIC_OK;
}

/* Register a counter metric */
distric_err_t metrics_register_counter(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
) {
    return register_metric(registry, name, help, METRIC_TYPE_COUNTER, 
                          labels, label_count, out_metric);
}

/* Register a gauge metric */
distric_err_t metrics_register_gauge(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
) {
    return register_metric(registry, name, help, METRIC_TYPE_GAUGE,
                          labels, label_count, out_metric);
}

/* Register a histogram metric */
distric_err_t metrics_register_histogram(
    metrics_registry_t* registry,
    const char* name,
    const char* help,
    const metric_label_t* labels,
    size_t label_count,
    metric_t** out_metric
) {
    return register_metric(registry, name, help, METRIC_TYPE_HISTOGRAM,
                          labels, label_count, out_metric);
}

/* Increment counter by 1 */
void metrics_counter_inc(metric_t* metric) {
    if (metric && metric->type == METRIC_TYPE_COUNTER) {
        atomic_fetch_add(&metric->data.counter.value, 1);
    }
}

/* Increment counter by value */
void metrics_counter_add(metric_t* metric, uint64_t value) {
    if (metric && metric->type == METRIC_TYPE_COUNTER) {
        atomic_fetch_add(&metric->data.counter.value, value);
    }
}

/* Set gauge to value */
void metrics_gauge_set(metric_t* metric, double value) {
    if (metric && metric->type == METRIC_TYPE_GAUGE) {
        atomic_store(&metric->data.gauge.bits, double_to_bits(value));
    }
}

/* Record observation in histogram */
void metrics_histogram_observe(metric_t* metric, double value) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) {
        return;
    }
    
    /* FIX #5: Document that histogram updates are eventually consistent */
    /* NOTE: Updates are not transactional. In high-concurrency scenarios,
     * an exporter may observe a count that temporarily doesn't match the 
     * sum of buckets. This is an acceptable tradeoff for lock-free performance.
     * The inconsistency is typically sub-microsecond and resolves naturally.
     */
    
    /* Update count */
    atomic_fetch_add(&metric->data.histogram.count, 1);
    
    /* Update sum atomically using compare-exchange */
    uint64_t old_sum_bits = atomic_load(&metric->data.histogram.sum_bits);
    uint64_t new_sum_bits;
    do {
        double old_sum = bits_to_double(old_sum_bits);
        double new_sum = old_sum + value;
        new_sum_bits = double_to_bits(new_sum);
    } while (!atomic_compare_exchange_weak(&metric->data.histogram.sum_bits,
                                           &old_sum_bits, new_sum_bits));
    
    /* Update appropriate bucket */
    for (int i = 0; i < HISTOGRAM_BUCKET_COUNT; i++) {
        if (value <= HISTOGRAM_BUCKETS[i]) {
            atomic_fetch_add(&metric->data.histogram.buckets[i], 1);
            break;
        }
    }
}

/* Helper: Escape string for Prometheus format */
static void escape_label_value(const char* src, char* dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 2; i++) {
        if (src[i] == '\\' || src[i] == '"' || src[i] == '\n') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
            }
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Export all metrics in Prometheus text format */
distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char** out_buffer,
    size_t* out_size
) {
    if (!registry || !out_buffer || !out_size) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t buffer_size = 1024 * 1024;  /* 1MB initial size */
    char* buffer = malloc(buffer_size);
    if (!buffer) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    size_t offset = 0;
    size_t count = atomic_load(&registry->metric_count);
    
    for (size_t i = 0; i < count; i++) {
        metric_t* m = &registry->metrics[i];
        
        /* FIX #3: Skip uninitialized metrics */
        if (!atomic_load(&m->initialized)) {
            continue;
        }
        
        /* HELP line */
        int written = snprintf(buffer + offset, buffer_size - offset,
                              "# HELP %s %s\n", m->name, m->help);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        
        /* TYPE line */
        const char* type_str = "untyped";
        if (m->type == METRIC_TYPE_COUNTER) type_str = "counter";
        else if (m->type == METRIC_TYPE_GAUGE) type_str = "gauge";
        else if (m->type == METRIC_TYPE_HISTOGRAM) type_str = "histogram";
        
        written = snprintf(buffer + offset, buffer_size - offset,
                          "# TYPE %s %s\n", m->name, type_str);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        
        /* Build label string */
        char labels[1024] = "";
        if (m->label_count > 0) {
            size_t label_offset = 0;
            labels[label_offset++] = '{';
            
            for (size_t j = 0; j < m->label_count; j++) {
                char escaped_value[MAX_LABEL_VALUE_LEN * 2];
                escape_label_value(m->labels[j].value, escaped_value, sizeof(escaped_value));
                
                int l = snprintf(labels + label_offset, sizeof(labels) - label_offset,
                               "%s%s=\"%s\"", j > 0 ? "," : "",
                               m->labels[j].key, escaped_value);
                if (l > 0) label_offset += l;
            }
            labels[label_offset++] = '}';
            labels[label_offset] = '\0';
        }
        
        /* Metric value(s) */
        if (m->type == METRIC_TYPE_COUNTER) {
            uint64_t value = atomic_load(&m->data.counter.value);
            written = snprintf(buffer + offset, buffer_size - offset,
                             "%s%s %lu\n", m->name, labels, value);
        } else if (m->type == METRIC_TYPE_GAUGE) {
            uint64_t bits = atomic_load(&m->data.gauge.bits);
            double value = bits_to_double(bits);
            written = snprintf(buffer + offset, buffer_size - offset,
                             "%s%s %.6f\n", m->name, labels, value);
        } else if (m->type == METRIC_TYPE_HISTOGRAM) {
            /* Histogram buckets */
            for (int b = 0; b < HISTOGRAM_BUCKET_COUNT; b++) {
                uint64_t count = atomic_load(&m->data.histogram.buckets[b]);
                char bucket_labels[1200];
                
                if (m->label_count > 0) {
                    snprintf(bucket_labels, sizeof(bucket_labels), "{le=\"%s\",%s",
                            isinf(HISTOGRAM_BUCKETS[b]) ? "+Inf" : "",
                            labels + 1);  /* Skip opening { */
                    if (!isinf(HISTOGRAM_BUCKETS[b])) {
                        char le_str[32];
                        snprintf(le_str, sizeof(le_str), "%.3f", HISTOGRAM_BUCKETS[b]);
                        snprintf(bucket_labels, sizeof(bucket_labels), "{le=\"%s\",%s",
                                le_str, labels + 1);
                    }
                } else {
                    if (isinf(HISTOGRAM_BUCKETS[b])) {
                        snprintf(bucket_labels, sizeof(bucket_labels), "{le=\"+Inf\"}");
                    } else {
                        snprintf(bucket_labels, sizeof(bucket_labels), "{le=\"%.3f\"}",
                                HISTOGRAM_BUCKETS[b]);
                    }
                }
                
                written = snprintf(buffer + offset, buffer_size - offset,
                                 "%s_bucket%s %lu\n", m->name, bucket_labels, count);
                if (written < 0 || (size_t)written >= buffer_size - offset) {
                    free(buffer);
                    return DISTRIC_ERR_BUFFER_OVERFLOW;
                }
                offset += written;
            }
            
            /* Sum and count */
            uint64_t sum_bits = atomic_load(&m->data.histogram.sum_bits);
            double sum = bits_to_double(sum_bits);
            uint64_t count = atomic_load(&m->data.histogram.count);
            
            written = snprintf(buffer + offset, buffer_size - offset,
                             "%s_sum%s %.6f\n%s_count%s %lu\n",
                             m->name, labels, sum, m->name, labels, count);
        }
        
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
    }
    
    *out_buffer = buffer;
    *out_size = offset;
    
    return DISTRIC_OK;
}