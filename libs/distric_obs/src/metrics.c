/*
 * metrics.c — DistriC Observability Library — Metrics Implementation
 *
 * =============================================================================
 * LABEL CARDINALITY — STRICT ENFORCEMENT
 * =============================================================================
 *
 * ALL label allowlist enforcement is unconditionally strict:
 *
 *   1. Registration: a label dimension with num_allowed_values == 0 or
 *      allowed_values == NULL is UNBOUNDED.  compute_cardinality() returns 0,
 *      and registration fails with DISTRIC_ERR_HIGH_CARDINALITY.
 *
 *   2. Update-time: validate_label() rejects values outside the allowlist
 *      (defence-in-depth; correctly registered metrics should not reach this).
 *
 *   3. The effective cardinality cap comes from metrics_config_t.max_cardinality
 *      and is validated at init time.
 *
 * Thread-safety:
 *   - metrics_init / metrics_destroy: caller must serialize.
 *   - metrics_register_*: serialised by registry->register_mutex.
 *   - counter_inc / gauge_set / histogram_observe: lock-free atomics.
 *   - metrics_export_prometheus: safe after metrics_freeze().
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include "distric_obs/metrics.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>
#include <assert.h>

/* ============================================================================
 * Internal constants
 * ========================================================================= */

static const double HISTOGRAM_DEFAULT_BUCKETS[] = {
    0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, INFINITY
};

/* ============================================================================
 * Cardinality helpers
 * ========================================================================= */

/*
 * Returns 0 if any dimension is unbounded or product overflows cap.
 * Returns 1 for an unlabelled metric.
 * STRICT: NULL or zero-size allowlist is always unbounded.
 */
static size_t compute_cardinality(const metric_label_definition_t* defs,
                                   size_t count) {
    if (count == 0) return 1;
    size_t card = 1;
    for (size_t i = 0; i < count; i++) {
        if (!defs[i].allowed_values || defs[i].num_allowed_values == 0) return 0;
        /* overflow-safe multiply */
        if (card > SIZE_MAX / defs[i].num_allowed_values) return 0;
        card *= defs[i].num_allowed_values;
    }
    return card;
}

static bool validate_label(const metric_label_definition_t* def,
                            const char* value) {
    if (!def->allowed_values || def->num_allowed_values == 0) return false;
    for (size_t i = 0; i < def->num_allowed_values; i++) {
        if (def->allowed_values[i] && strcmp(def->allowed_values[i], value) == 0)
            return true;
    }
    return false;
}

/* ============================================================================
 * Init / Destroy / Lifecycle
 * ========================================================================= */

distric_err_t metrics_init(metrics_registry_t** registry) {
    metrics_config_t cfg = { 0 };
    return metrics_init_with_config(registry, &cfg);
}

distric_err_t metrics_init_with_config(metrics_registry_t** registry,
                                         const metrics_config_t* config) {
    if (!registry) return DISTRIC_ERR_INVALID_ARG;

    metrics_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) return DISTRIC_ERR_ALLOC_FAILURE;

    /* Apply config with safe defaults and hard-cap enforcement */
    size_t max = config ? config->max_metrics : 0;
    if (max == 0 || max > DISTRIC_MAX_METRICS) max = DISTRIC_MAX_METRICS;
    reg->effective_max = max;

    size_t card_cap = config ? config->max_cardinality : 0;
    if (card_cap == 0 || card_cap > DISTRIC_MAX_METRIC_CARDINALITY)
        card_cap = DISTRIC_MAX_METRIC_CARDINALITY;
    reg->effective_cardinality_cap = card_cap;

    atomic_init(&reg->metric_count, 0);
    atomic_init(&reg->state, (uint32_t)REGISTRY_STATE_MUTABLE);
    atomic_init(&reg->refcount, 1);

    if (pthread_mutex_init(&reg->register_mutex, NULL) != 0) {
        free(reg);
        return DISTRIC_ERR_INIT_FAILED;
    }

    /* Initialize all metric initialized flags */
    for (size_t i = 0; i < DISTRIC_MAX_METRICS; i++)
        atomic_init(&reg->metrics[i].initialized, false);

    *registry = reg;
    return DISTRIC_OK;
}

void metrics_retain(metrics_registry_t* registry) {
    if (!registry) return;
    METRICS_ASSERT_LIFECYCLE(
        atomic_load_explicit(&registry->refcount, memory_order_relaxed) > 0);
    atomic_fetch_add_explicit(&registry->refcount, 1, memory_order_relaxed);
}

void metrics_release(metrics_registry_t* registry) {
    if (!registry) return;
    uint32_t prev = atomic_fetch_sub_explicit(&registry->refcount, 1,
                                               memory_order_acq_rel);
    METRICS_ASSERT_LIFECYCLE(prev > 0);
    if (prev == 1) {
        metrics_destroy(registry);
    }
}

void metrics_destroy(metrics_registry_t* registry) {
    if (!registry) return;

    /* Allow destroy to be called directly (not via release) */
    atomic_store_explicit(&registry->state,
                          (uint32_t)REGISTRY_STATE_DESTROYED,
                          memory_order_release);

    size_t n = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    for (size_t i = 0; i < n; i++) {
        metric_t* m = &registry->metrics[i];
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) continue;

        if (m->type == METRIC_TYPE_COUNTER) {
            counter_instance_t* inst =
                atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
            while (inst) {
                counter_instance_t* next = inst->next;
                free(inst);
                inst = next;
            }
            pthread_mutex_destroy(&m->data.counter.instance_lock);
        } else if (m->type == METRIC_TYPE_GAUGE) {
            gauge_instance_t* inst =
                atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
            while (inst) {
                gauge_instance_t* next = inst->next;
                free(inst);
                inst = next;
            }
            pthread_mutex_destroy(&m->data.gauge.instance_lock);
        } else {
            histogram_instance_t* inst =
                atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
            while (inst) {
                histogram_instance_t* next = inst->next;
                free(inst->buckets);
                free(inst);
                inst = next;
            }
            pthread_mutex_destroy(&m->data.histogram.instance_lock);
        }
    }

    pthread_mutex_destroy(&registry->register_mutex);
    free(registry);
}

void metrics_freeze(metrics_registry_t* registry) {
    if (!registry) return;
    atomic_store_explicit(&registry->state,
                          (uint32_t)REGISTRY_STATE_FROZEN,
                          memory_order_release);
}

/* ============================================================================
 * Registration
 * ========================================================================= */

static distric_err_t register_metric(metrics_registry_t* registry,
                                      const char*          name,
                                      const char*          help,
                                      metric_type_t        type,
                                      const metric_label_definition_t* label_defs,
                                      size_t               label_def_count,
                                      metric_t**           out_metric) {
    if (!registry || !name || !help || !out_metric) return DISTRIC_ERR_INVALID_ARG;
    if (label_def_count > MAX_METRIC_LABELS)         return DISTRIC_ERR_INVALID_ARG;

    /* Validate cardinality before acquiring mutex */
    size_t card = compute_cardinality(label_defs, label_def_count);
    if (card == 0 && label_def_count > 0)
        return DISTRIC_ERR_HIGH_CARDINALITY;
    if (card > registry->effective_cardinality_cap)
        return DISTRIC_ERR_HIGH_CARDINALITY;

    pthread_mutex_lock(&registry->register_mutex);

    registry_state_t state =
        (registry_state_t)atomic_load_explicit(&registry->state, memory_order_acquire);
    if (state == REGISTRY_STATE_FROZEN) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }
    if (state == REGISTRY_STATE_DESTROYED) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_SHUTDOWN;
    }

    size_t idx = atomic_load_explicit(&registry->metric_count, memory_order_relaxed);
    if (idx >= registry->effective_max) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }

    metric_t* m = &registry->metrics[idx];
    memset(m, 0, sizeof(*m));

    strncpy(m->name, name, MAX_METRIC_NAME_LEN - 1);
    strncpy(m->help, help, MAX_METRIC_HELP_LEN - 1);
    m->type = type;
    m->num_label_defs = (uint32_t)(label_def_count < MAX_METRIC_LABELS
                                   ? label_def_count : MAX_METRIC_LABELS);
    if (label_defs && label_def_count > 0)
        memcpy(m->label_defs, label_defs,
               m->num_label_defs * sizeof(metric_label_definition_t));

    if (type == METRIC_TYPE_COUNTER) {
        atomic_init(&m->data.counter.instances, NULL);
        pthread_mutex_init(&m->data.counter.instance_lock, NULL);
    } else if (type == METRIC_TYPE_GAUGE) {
        atomic_init(&m->data.gauge.instances, NULL);
        pthread_mutex_init(&m->data.gauge.instance_lock, NULL);
    } else {
        atomic_init(&m->data.histogram.instances, NULL);
        pthread_mutex_init(&m->data.histogram.instance_lock, NULL);
        m->data.histogram.num_buckets = HISTOGRAM_BUCKET_COUNT;
        memcpy(m->data.histogram.buckets_template, HISTOGRAM_DEFAULT_BUCKETS,
               sizeof(HISTOGRAM_DEFAULT_BUCKETS));
    }

    atomic_init(&m->initialized, true);
    atomic_store_explicit(&registry->metric_count, idx + 1, memory_order_release);
    pthread_mutex_unlock(&registry->register_mutex);

    *out_metric = m;
    return DISTRIC_OK;
}

distric_err_t metrics_register_counter(metrics_registry_t* r, const char* name,
    const char* help, const metric_label_definition_t* ld, size_t lc,
    metric_t** out) {
    return register_metric(r, name, help, METRIC_TYPE_COUNTER, ld, lc, out);
}

distric_err_t metrics_register_gauge(metrics_registry_t* r, const char* name,
    const char* help, const metric_label_definition_t* ld, size_t lc,
    metric_t** out) {
    return register_metric(r, name, help, METRIC_TYPE_GAUGE, ld, lc, out);
}

distric_err_t metrics_register_histogram(metrics_registry_t* r, const char* name,
    const char* help, const metric_label_definition_t* ld, size_t lc,
    metric_t** out) {
    return register_metric(r, name, help, METRIC_TYPE_HISTOGRAM, ld, lc, out);
}

/* ============================================================================
 * Instance lookup / creation helpers
 * ========================================================================= */

static bool labels_match(const metric_label_t* a, uint32_t na,
                          const metric_label_t* b, uint32_t nb) {
    if (na != nb) return false;
    for (uint32_t i = 0; i < na; i++) {
        if (strcmp(a[i].key, b[i].key) != 0 ||
            strcmp(a[i].value, b[i].value) != 0)
            return false;
    }
    return true;
}

static bool validate_labels(const metric_t* m,
                              const metric_label_t* labels, uint32_t num_labels) {
    if (m->num_label_defs == 0 && num_labels == 0) return true;
    if (num_labels != m->num_label_defs) return false;
    for (uint32_t i = 0; i < num_labels; i++) {
        bool found = false;
        for (uint32_t d = 0; d < m->num_label_defs; d++) {
            if (strcmp(labels[i].key, m->label_defs[d].key) == 0) {
                if (!validate_label(&m->label_defs[d], labels[i].value))
                    return false;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* ============================================================================
 * Counter operations
 * ========================================================================= */

static counter_instance_t* get_or_create_counter_instance(
    metric_t* m, const metric_label_t* labels, uint32_t num_labels) {

    /* Fast path: search existing (lock-free read of linked list) */
    counter_instance_t* inst =
        atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels))
            return inst;
        inst = inst->next;
    }

    /* Slow path: create new instance (mutex-protected) */
    pthread_mutex_lock(&m->data.counter.instance_lock);

    /* Re-check after lock (another thread may have created it) */
    inst = atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels)) {
            pthread_mutex_unlock(&m->data.counter.instance_lock);
            return inst;
        }
        inst = inst->next;
    }

    counter_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&m->data.counter.instance_lock);
        return NULL;
    }

    if (labels && num_labels > 0)
        memcpy(new_inst->labels, labels,
               num_labels * sizeof(metric_label_t));
    new_inst->num_labels = num_labels;
    atomic_init(&new_inst->value, 0);

    /* Prepend to list (store-release for visibility) */
    counter_instance_t* head =
        atomic_load_explicit(&m->data.counter.instances, memory_order_relaxed);
    new_inst->next = head;
    atomic_store_explicit(&m->data.counter.instances, new_inst, memory_order_release);

    pthread_mutex_unlock(&m->data.counter.instance_lock);
    return new_inst;
}

void metrics_counter_inc(metric_t* metric) {
    if (!metric) return;
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances, memory_order_acquire);
    if (inst)
        atomic_fetch_add_explicit(&inst->value, 1, memory_order_relaxed);
    else
        get_or_create_counter_instance(metric, NULL, 0);
}

void metrics_counter_add(metric_t* metric, uint64_t value) {
    if (!metric) return;
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances, memory_order_acquire);
    if (inst)
        atomic_fetch_add_explicit(&inst->value, value, memory_order_relaxed);
    else {
        inst = get_or_create_counter_instance(metric, NULL, 0);
        if (inst)
            atomic_fetch_add_explicit(&inst->value, value, memory_order_relaxed);
    }
}

distric_err_t metrics_counter_inc_labels(metric_t* metric,
                                          const metric_label_t* labels,
                                          uint32_t num_labels) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    if (!validate_labels(metric, labels, num_labels)) return DISTRIC_ERR_INVALID_LABEL;

    counter_instance_t* inst =
        get_or_create_counter_instance(metric, labels, num_labels);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;
    atomic_fetch_add_explicit(&inst->value, 1, memory_order_relaxed);
    return DISTRIC_OK;
}

distric_err_t metrics_counter_add_labels(metric_t* metric,
                                          const metric_label_t* labels,
                                          uint32_t num_labels, uint64_t value) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    if (!validate_labels(metric, labels, num_labels)) return DISTRIC_ERR_INVALID_LABEL;

    counter_instance_t* inst =
        get_or_create_counter_instance(metric, labels, num_labels);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;
    atomic_fetch_add_explicit(&inst->value, value, memory_order_relaxed);
    return DISTRIC_OK;
}

uint64_t metrics_counter_get(metric_t* metric) {
    if (!metric) return 0;
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances, memory_order_acquire);
    if (!inst) return 0;
    return atomic_load_explicit(&inst->value, memory_order_relaxed);
}

/* ============================================================================
 * Gauge operations
 * ========================================================================= */

static inline uint64_t double_to_bits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static inline double bits_to_double(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static gauge_instance_t* get_or_create_gauge_instance(
    metric_t* m, const metric_label_t* labels, uint32_t num_labels) {

    gauge_instance_t* inst =
        atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels))
            return inst;
        inst = inst->next;
    }

    pthread_mutex_lock(&m->data.gauge.instance_lock);

    inst = atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels)) {
            pthread_mutex_unlock(&m->data.gauge.instance_lock);
            return inst;
        }
        inst = inst->next;
    }

    gauge_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&m->data.gauge.instance_lock);
        return NULL;
    }

    if (labels && num_labels > 0)
        memcpy(new_inst->labels, labels, num_labels * sizeof(metric_label_t));
    new_inst->num_labels = num_labels;
    atomic_init(&new_inst->value_bits, 0);

    gauge_instance_t* head =
        atomic_load_explicit(&m->data.gauge.instances, memory_order_relaxed);
    new_inst->next = head;
    atomic_store_explicit(&m->data.gauge.instances, new_inst, memory_order_release);

    pthread_mutex_unlock(&m->data.gauge.instance_lock);
    return new_inst;
}

void metrics_gauge_set(metric_t* metric, double value) {
    if (!metric) return;
    gauge_instance_t* inst =
        atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    if (!inst)
        inst = get_or_create_gauge_instance(metric, NULL, 0);
    if (inst)
        atomic_store_explicit(&inst->value_bits, double_to_bits(value),
                              memory_order_relaxed);
}

distric_err_t metrics_gauge_set_labels(metric_t* metric,
                                        const metric_label_t* labels,
                                        uint32_t num_labels, double value) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    if (!validate_labels(metric, labels, num_labels)) return DISTRIC_ERR_INVALID_LABEL;

    gauge_instance_t* inst =
        get_or_create_gauge_instance(metric, labels, num_labels);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;
    atomic_store_explicit(&inst->value_bits, double_to_bits(value),
                          memory_order_relaxed);
    return DISTRIC_OK;
}

double metrics_gauge_get(metric_t* metric) {
    if (!metric) return 0.0;
    gauge_instance_t* inst =
        atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    if (!inst) return 0.0;
    return bits_to_double(
        atomic_load_explicit(&inst->value_bits, memory_order_relaxed));
}

/* ============================================================================
 * Histogram operations
 * ========================================================================= */

static histogram_instance_t* get_or_create_histogram_instance(
    metric_t* m, const metric_label_t* labels, uint32_t num_labels) {

    histogram_instance_t* inst =
        atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels))
            return inst;
        inst = inst->next;
    }

    pthread_mutex_lock(&m->data.histogram.instance_lock);

    inst = atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels)) {
            pthread_mutex_unlock(&m->data.histogram.instance_lock);
            return inst;
        }
        inst = inst->next;
    }

    histogram_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&m->data.histogram.instance_lock);
        return NULL;
    }

    uint32_t nb = m->data.histogram.num_buckets;
    new_inst->buckets = calloc(nb, sizeof(histogram_bucket_t));
    if (!new_inst->buckets) {
        free(new_inst);
        pthread_mutex_unlock(&m->data.histogram.instance_lock);
        return NULL;
    }

    for (uint32_t i = 0; i < nb; i++) {
        new_inst->buckets[i].upper_bound = m->data.histogram.buckets_template[i];
        atomic_init(&new_inst->buckets[i].count, 0);
    }

    if (labels && num_labels > 0)
        memcpy(new_inst->labels, labels, num_labels * sizeof(metric_label_t));
    new_inst->num_labels  = num_labels;
    new_inst->num_buckets = nb;
    atomic_init(&new_inst->count, 0);
    atomic_init(&new_inst->sum_bits, 0);

    histogram_instance_t* head =
        atomic_load_explicit(&m->data.histogram.instances, memory_order_relaxed);
    new_inst->next = head;
    atomic_store_explicit(&m->data.histogram.instances, new_inst, memory_order_release);

    pthread_mutex_unlock(&m->data.histogram.instance_lock);
    return new_inst;
}

void metrics_histogram_observe(metric_t* metric, double value) {
    if (!metric) return;
    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances, memory_order_acquire);
    if (!inst)
        inst = get_or_create_histogram_instance(metric, NULL, 0);
    if (!inst) return;

    atomic_fetch_add_explicit(&inst->count, 1, memory_order_relaxed);

    /* Accumulate sum via CAS loop (double stored as uint64 bits) */
    uint64_t old_bits, new_bits;
    do {
        old_bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
        double new_sum = bits_to_double(old_bits) + value;
        new_bits = double_to_bits(new_sum);
    } while (!atomic_compare_exchange_weak_explicit(
        &inst->sum_bits, &old_bits, new_bits,
        memory_order_relaxed, memory_order_relaxed));

    for (uint32_t i = 0; i < inst->num_buckets; i++) {
        if (value <= inst->buckets[i].upper_bound)
            atomic_fetch_add_explicit(&inst->buckets[i].count, 1, memory_order_relaxed);
    }
}

distric_err_t metrics_histogram_observe_labels(metric_t* metric,
                                                const metric_label_t* labels,
                                                uint32_t num_labels,
                                                double value) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    if (!validate_labels(metric, labels, num_labels)) return DISTRIC_ERR_INVALID_LABEL;

    histogram_instance_t* inst =
        get_or_create_histogram_instance(metric, labels, num_labels);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;

    atomic_fetch_add_explicit(&inst->count, 1, memory_order_relaxed);
    uint64_t old_bits, new_bits;
    do {
        old_bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
        double ns = bits_to_double(old_bits) + value;
        new_bits  = double_to_bits(ns);
    } while (!atomic_compare_exchange_weak_explicit(
        &inst->sum_bits, &old_bits, new_bits,
        memory_order_relaxed, memory_order_relaxed));

    for (uint32_t i = 0; i < inst->num_buckets; i++) {
        if (value <= inst->buckets[i].upper_bound)
            atomic_fetch_add_explicit(&inst->buckets[i].count, 1, memory_order_relaxed);
    }
    return DISTRIC_OK;
}

uint64_t metrics_histogram_get_count(metric_t* metric) {
    if (!metric) return 0;
    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances, memory_order_acquire);
    if (!inst) return 0;
    return atomic_load_explicit(&inst->count, memory_order_relaxed);
}

double metrics_histogram_get_sum(metric_t* metric) {
    if (!metric) return 0.0;
    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances, memory_order_acquire);
    if (!inst) return 0.0;
    return bits_to_double(
        atomic_load_explicit(&inst->sum_bits, memory_order_relaxed));
}

/* ============================================================================
 * Prometheus export
 * ========================================================================= */

/*
 * ENSURE_SPACE: grow buffer if needed.  Macro for clarity; only used in
 * metrics_export_prometheus.
 */
#define ENSURE_SPACE(needed)                                               \
    while (offset + (needed) > buf_size) {                                 \
        buf_size *= 2;                                                     \
        char* nb = realloc(buf, buf_size);                                 \
        if (!nb) { free(buf); return DISTRIC_ERR_NO_MEMORY; }             \
        buf = nb;                                                          \
    }

static void append_label_set(char** buf_ptr, size_t* offset_ptr, size_t* buf_size_ptr,
                               const metric_label_t* labels, uint32_t num_labels) {
    char*  buf      = *buf_ptr;
    size_t offset   = *offset_ptr;
    size_t buf_size = *buf_size_ptr;

    if (num_labels == 0) return;
    int w = snprintf(buf + offset, buf_size - offset, "{");
    if (w > 0) offset += (size_t)w;
    for (uint32_t li = 0; li < num_labels; li++) {
        w = snprintf(buf + offset, buf_size - offset,
                     "%s%s=\"%s\"",
                     li ? "," : "",
                     labels[li].key, labels[li].value);
        if (w > 0) offset += (size_t)w;
    }
    w = snprintf(buf + offset, buf_size - offset, "}");
    if (w > 0) offset += (size_t)w;

    *buf_ptr      = buf;
    *offset_ptr   = offset;
    *buf_size_ptr = buf_size;
    (void)buf_size; /* suppress unused warning */
}

distric_err_t metrics_export_prometheus(metrics_registry_t* registry,
                                         char** out_buffer, size_t* out_size) {
    if (!registry || !out_buffer || !out_size) return DISTRIC_ERR_INVALID_ARG;

    size_t buf_size = 65536;
    char*  buf = malloc(buf_size);
    if (!buf) return DISTRIC_ERR_NO_MEMORY;
    size_t offset = 0;

    size_t n = atomic_load_explicit(&registry->metric_count, memory_order_acquire);

    for (size_t mi = 0; mi < n; mi++) {
        metric_t* m = &registry->metrics[mi];
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) continue;

        ENSURE_SPACE(512 + MAX_METRIC_NAME_LEN + MAX_METRIC_HELP_LEN);
        int w = snprintf(buf + offset, buf_size - offset,
                         "# HELP %s %s\n", m->name, m->help);
        if (w > 0) offset += (size_t)w;

        const char* type_str =
            m->type == METRIC_TYPE_COUNTER   ? "counter"   :
            m->type == METRIC_TYPE_GAUGE     ? "gauge"     : "histogram";

        w = snprintf(buf + offset, buf_size - offset,
                     "# TYPE %s %s\n", m->name, type_str);
        if (w > 0) offset += (size_t)w;

        if (m->type == METRIC_TYPE_COUNTER) {
            counter_instance_t* inst =
                atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
            if (!inst) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 32);
                w = snprintf(buf + offset, buf_size - offset, "%s 0\n", m->name);
                if (w > 0) offset += (size_t)w;
            }
            while (inst) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN +
                             MAX_METRIC_LABELS * (MAX_LABEL_KEY_LEN + MAX_LABEL_VALUE_LEN + 6) +
                             64);
                w = snprintf(buf + offset, buf_size - offset, "%s", m->name);
                if (w > 0) offset += (size_t)w;

                append_label_set(&buf, &offset, &buf_size,
                                  inst->labels, inst->num_labels);

                uint64_t val =
                    atomic_load_explicit(&inst->value, memory_order_relaxed);
                w = snprintf(buf + offset, buf_size - offset, " %lu\n",
                             (unsigned long)val);
                if (w > 0) offset += (size_t)w;
                inst = inst->next;
            }

        } else if (m->type == METRIC_TYPE_GAUGE) {
            gauge_instance_t* inst =
                atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
            if (!inst) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 32);
                w = snprintf(buf + offset, buf_size - offset, "%s 0\n", m->name);
                if (w > 0) offset += (size_t)w;
            }
            while (inst) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN +
                             MAX_METRIC_LABELS * (MAX_LABEL_KEY_LEN + MAX_LABEL_VALUE_LEN + 6) +
                             64);
                w = snprintf(buf + offset, buf_size - offset, "%s", m->name);
                if (w > 0) offset += (size_t)w;

                append_label_set(&buf, &offset, &buf_size,
                                  inst->labels, inst->num_labels);

                double val = bits_to_double(
                    atomic_load_explicit(&inst->value_bits, memory_order_relaxed));
                w = snprintf(buf + offset, buf_size - offset, " %g\n", val);
                if (w > 0) offset += (size_t)w;
                inst = inst->next;
            }

        } else { /* HISTOGRAM */
            histogram_instance_t* inst =
                atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
            if (!inst) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 64);
                w = snprintf(buf + offset, buf_size - offset,
                             "%s_count 0\n%s_sum 0\n", m->name, m->name);
                if (w > 0) offset += (size_t)w;
            }
            while (inst) {
                for (uint32_t bi = 0; bi < inst->num_buckets; bi++) {
                    ENSURE_SPACE(MAX_METRIC_NAME_LEN + 256 + 64);
                    const char* ub_str =
                        isinf(inst->buckets[bi].upper_bound) ? "+Inf" : NULL;
                    char ub_buf[32];
                    if (!ub_str) {
                        snprintf(ub_buf, sizeof(ub_buf), "%g",
                                 inst->buckets[bi].upper_bound);
                        ub_str = ub_buf;
                    }

                    w = snprintf(buf + offset, buf_size - offset,
                                 "%s_bucket", m->name);
                    if (w > 0) offset += (size_t)w;

                    /* Label set including le */
                    if (inst->num_labels > 0) {
                        w = snprintf(buf + offset, buf_size - offset, "{");
                        if (w > 0) offset += (size_t)w;
                        for (uint32_t li = 0; li < inst->num_labels; li++) {
                            w = snprintf(buf + offset, buf_size - offset,
                                         "%s=\"%s\",",
                                         inst->labels[li].key,
                                         inst->labels[li].value);
                            if (w > 0) offset += (size_t)w;
                        }
                        w = snprintf(buf + offset, buf_size - offset,
                                     "le=\"%s\"}", ub_str);
                    } else {
                        w = snprintf(buf + offset, buf_size - offset,
                                     "{le=\"%s\"}", ub_str);
                    }
                    if (w > 0) offset += (size_t)w;

                    uint64_t bc =
                        atomic_load_explicit(&inst->buckets[bi].count,
                                            memory_order_relaxed);
                    w = snprintf(buf + offset, buf_size - offset, " %lu\n",
                                 (unsigned long)bc);
                    if (w > 0) offset += (size_t)w;
                }

                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 256 + 64);
                uint64_t cnt =
                    atomic_load_explicit(&inst->count, memory_order_relaxed);
                double sum =
                    bits_to_double(
                        atomic_load_explicit(&inst->sum_bits, memory_order_relaxed));

                w = snprintf(buf + offset, buf_size - offset,
                             "%s_count", m->name);
                if (w > 0) offset += (size_t)w;
                append_label_set(&buf, &offset, &buf_size,
                                  inst->labels, inst->num_labels);
                w = snprintf(buf + offset, buf_size - offset, " %lu\n",
                             (unsigned long)cnt);
                if (w > 0) offset += (size_t)w;

                w = snprintf(buf + offset, buf_size - offset,
                             "%s_sum", m->name);
                if (w > 0) offset += (size_t)w;
                append_label_set(&buf, &offset, &buf_size,
                                  inst->labels, inst->num_labels);
                w = snprintf(buf + offset, buf_size - offset, " %g\n", sum);
                if (w > 0) offset += (size_t)w;

                inst = inst->next;
            }
        }
    }

    buf[offset] = '\0';
    *out_buffer = buf;
    *out_size   = offset;
    return DISTRIC_OK;
}