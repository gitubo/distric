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
    
    /* Initialize mutex for registration */
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
    size_t count = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    
    for (size_t i = 0; i < count; i++) {
        metric_t* m = &registry->metrics[i];
        
        /* Only check initialized metrics with proper memory ordering */
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) {
            continue;
        }
        
        /* Memory fence to ensure we see complete metric data */
        atomic_thread_fence(memory_order_acquire);
        
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

/* Helper: Register a metric with common logic - FIXED */
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
    
    /* Validate name and help lengths */
    if (strlen(name) >= MAX_METRIC_NAME_LEN || 
        strlen(help) >= MAX_METRIC_HELP_LEN) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* CRITICAL FIX: Lock for entire registration process */
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
    size_t idx = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    metric_t* metric = &registry->metrics[idx];
    
    /* CRITICAL FIX: Mark as initializing to prevent partial reads */
    atomic_store_explicit(&metric->initialized, false, memory_order_release);
    
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
    
    /* CRITICAL FIX: Memory fence before marking as initialized */
    atomic_thread_fence(memory_order_release);
    
    /* Mark as initialized - MUST be AFTER all data is written */
    atomic_store_explicit(&metric->initialized, true, memory_order_release);
    
    /* Increment count AFTER metric is fully initialized */
    atomic_store_explicit(&registry->metric_count, idx + 1, memory_order_release);
    
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
    if (!metric || metric->type != METRIC_TYPE_COUNTER) {
        return;
    }
    atomic_fetch_add_explicit(&metric->data.counter.value, 1, memory_order_relaxed);
}

/* Increment counter by value */
void metrics_counter_add(metric_t* metric, uint64_t value) {
    if (!metric || metric->type != METRIC_TYPE_COUNTER) {
        return;
    }
    atomic_fetch_add_explicit(&metric->data.counter.value, value, memory_order_relaxed);
}

/* Set gauge to value */
void metrics_gauge_set(metric_t* metric, double value) {
    if (!metric || metric->type != METRIC_TYPE_GAUGE) {
        return;
    }
    atomic_store_explicit(&metric->data.gauge.bits, double_to_bits(value), 
                         memory_order_relaxed);
}

/* Record observation in histogram - IMPROVED */
void metrics_histogram_observe(metric_t* metric, double value) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) {
        return;
    }
    
    /* IMPROVEMENT: Document eventual consistency tradeoff
     * 
     * Updates are lock-free but not transactional. Under high concurrency,
     * a reader may observe:
     * - count != sum of buckets (temporarily)
     * - sum that doesn't match actual observations (briefly)
     * 
     * This is acceptable because:
     * 1. Inconsistency window is sub-microsecond
     * 2. Values converge naturally
     * 3. Exporters typically sample at 1s+ intervals
     * 4. Alternative (locks) would degrade performance 10-100x
     */
    
    /* Update count - relaxed ordering is sufficient */
    atomic_fetch_add_explicit(&metric->data.histogram.count, 1, 
                             memory_order_relaxed);
    
    /* Update sum using compare-exchange
     * OPTIMIZATION NOTE: This CAS loop can contend under heavy load.
     * For 99th percentile latency requirements, consider:
     * - Thread-local accumulators with periodic flush
     * - Accept slightly stale sum values
     * - Use 128-bit CAS on platforms that support it
     */
    uint64_t old_sum_bits = atomic_load_explicit(&metric->data.histogram.sum_bits, 
                                                 memory_order_relaxed);
    uint64_t new_sum_bits;
    do {
        double old_sum = bits_to_double(old_sum_bits);
        double new_sum = old_sum + value;
        new_sum_bits = double_to_bits(new_sum);
    } while (!atomic_compare_exchange_weak_explicit(
        &metric->data.histogram.sum_bits,
        &old_sum_bits, new_sum_bits,
        memory_order_relaxed, memory_order_relaxed));
    
    /* Update appropriate bucket - find bucket and increment */
    for (int i = 0; i < HISTOGRAM_BUCKET_COUNT; i++) {
        if (value <= HISTOGRAM_BUCKETS[i]) {
            atomic_fetch_add_explicit(&metric->data.histogram.buckets[i], 1, 
                                     memory_order_relaxed);
            break;
        }
    }
}

/* Helper: Escape string for Prometheus format */
static void escape_label_value(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    
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

/* Export all metrics in Prometheus text format - IMPROVED */
distric_err_t metrics_export_prometheus(
    metrics_registry_t* registry,
    char** out_buffer,
    size_t* out_size
) {
    if (!registry || !out_buffer || !out_size) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* IMPROVEMENT: Estimate buffer size based on metric count */
    size_t count = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    size_t estimated_size = count * 512;  /* ~512 bytes per metric average */
    if (estimated_size < 4096) estimated_size = 4096;
    if (estimated_size > 1024 * 1024) estimated_size = 1024 * 1024;
    
    char* buffer = malloc(estimated_size);
    if (!buffer) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    size_t offset = 0;
    
    for (size_t i = 0; i < count; i++) {
        metric_t* m = &registry->metrics[i];
        
        /* Skip uninitialized metrics with proper memory ordering */
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) {
            continue;
        }
        
        /* Memory fence to ensure we see complete metric data */
        atomic_thread_fence(memory_order_acquire);
        
        /* HELP line */
        int written = snprintf(buffer + offset, estimated_size - offset,
                              "# HELP %s %s\n", m->name, m->help);
        if (written < 0 || (size_t)written >= estimated_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        
        /* TYPE line */
        const char* type_str = "untyped";
        if (m->type == METRIC_TYPE_COUNTER) type_str = "counter";
        else if (m->type == METRIC_TYPE_GAUGE) type_str = "gauge";
        else if (m->type == METRIC_TYPE_HISTOGRAM) type_str = "histogram";
        
        written = snprintf(buffer + offset, estimated_size - offset,
                          "# TYPE %s %s\n", m->name, type_str);
        if (written < 0 || (size_t)written >= estimated_size - offset) {
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
                escape_label_value(m->labels[j].value, escaped_value, 
                                  sizeof(escaped_value));
                
                int l = snprintf(labels + label_offset, sizeof(labels) - label_offset,
                               "%s%s=\"%s\"", j > 0 ? "," : "",
                               m->labels[j].key, escaped_value);
                if (l > 0) label_offset += l;
            }
            labels[label_offset++] = '}';
            labels[label_offset] = '\0';
        }
        
        /* Metric value(s) with proper memory ordering */
        if (m->type == METRIC_TYPE_COUNTER) {
            uint64_t value = atomic_load_explicit(&m->data.counter.value, 
                                                  memory_order_relaxed);
            written = snprintf(buffer + offset, estimated_size - offset,
                             "%s%s %lu\n", m->name, labels, value);
        } else if (m->type == METRIC_TYPE_GAUGE) {
            uint64_t bits = atomic_load_explicit(&m->data.gauge.bits, 
                                                memory_order_relaxed);
            double value = bits_to_double(bits);
            written = snprintf(buffer + offset, estimated_size - offset,
                             "%s%s %.6f\n", m->name, labels, value);
        } else if (m->type == METRIC_TYPE_HISTOGRAM) {
            /* Histogram buckets */
            for (int b = 0; b < HISTOGRAM_BUCKET_COUNT; b++) {
                uint64_t bucket_count = atomic_load_explicit(
                    &m->data.histogram.buckets[b], memory_order_relaxed);
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
                
                written = snprintf(buffer + offset, estimated_size - offset,
                                 "%s_bucket%s %lu\n", m->name, bucket_labels, 
                                 bucket_count);
                if (written < 0 || (size_t)written >= estimated_size - offset) {
                    free(buffer);
                    return DISTRIC_ERR_BUFFER_OVERFLOW;
                }
                offset += written;
            }
            
            /* Sum and count with proper memory ordering */
            uint64_t sum_bits = atomic_load_explicit(&m->data.histogram.sum_bits, 
                                                     memory_order_relaxed);
            double sum = bits_to_double(sum_bits);
            uint64_t hist_count = atomic_load_explicit(&m->data.histogram.count, 
                                                       memory_order_relaxed);
            
            written = snprintf(buffer + offset, estimated_size - offset,
                             "%s_sum%s %.6f\n%s_count%s %lu\n",
                             m->name, labels, sum, m->name, labels, hist_count);
        }
        
        if (written < 0 || (size_t)written >= estimated_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
    }
    
    *out_buffer = buffer;
    *out_size = offset;
    
    return DISTRIC_OK;
}