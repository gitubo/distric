/*
 * metrics.c — DistriC Observability Library — Metrics Implementation
 *
 * =============================================================================
 * ARCHITECTURE: FLAT CARTESIAN INSTANCE ARRAY (Item #4)
 * =============================================================================
 *
 * Instance storage uses a pre-allocated flat array of size `cardinality`
 * (= product of all allowed_values sizes) rather than a linked list.
 *
 * At REGISTRATION TIME (slow path, mutex-protected):
 *   1. compute_cardinality()     — validates all label dims are bounded.
 *   2. compute_strides()         — fills metric_t.strides[].
 *   3. Allocate instance_array   — one element per label combination.
 *   4. build_labels_table()      — decodes flat index → label set for export.
 *   5. For histograms:           — allocate and init buckets per instance.
 *   6. Initialize all atomics.
 *
 * At HOT-PATH TIME (lock-free, O(D) where D = number of label dimensions):
 *   1. compute_flat_index()      — maps label values → array index.
 *   2. instance_array[idx]       — direct access, no scan, no allocation.
 *
 * At EXPORT TIME:
 *   - Iterate 0..cardinality-1; read instance_array[fi] and look up the
 *     corresponding label set in labels_table.
 *
 * =============================================================================
 * LABEL CARDINALITY — STRICT ENFORCEMENT
 * =============================================================================
 *
 *   1. Registration: a label dimension with num_allowed_values == 0 or
 *      allowed_values == NULL is UNBOUNDED.  compute_cardinality() returns 0,
 *      and registration fails with DISTRIC_ERR_HIGH_CARDINALITY.
 *
 *   2. Update-time: compute_flat_index() returns -1 for any label value that
 *      is outside the registered allowlist.
 *
 *   3. The cardinality cap comes from metrics_config_t.max_cardinality
 *      and is enforced at registration time.
 *
 * Thread-safety:
 *   - metrics_init / metrics_destroy: caller must serialise.
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

static const double HISTOGRAM_DEFAULT_BUCKETS[HISTOGRAM_BUCKET_COUNT] = {
    0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, INFINITY
};

/* ============================================================================
 * Floating-point <-> uint64 bit-cast helpers
 *
 * Storing doubles in _Atomic uint64_t allows lock-free CAS loops.
 * memcpy is the only standards-conforming way to do this in C.
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

/* ============================================================================
 * Cardinality helpers
 * ========================================================================= */

/*
 * Returns 0 if any dimension is unbounded or product overflows.
 * Returns 1 for an unlabelled metric (count == 0).
 * STRICT: NULL or zero-size allowlist always counts as unbounded.
 */
static size_t compute_cardinality(const metric_label_definition_t* defs,
                                   size_t count) {
    if (count == 0) return 1;
    size_t card = 1;
    for (size_t i = 0; i < count; i++) {
        if (!defs[i].allowed_values || defs[i].num_allowed_values == 0)
            return 0;
        if (card > SIZE_MAX / defs[i].num_allowed_values)
            return 0;  /* overflow */
        card *= defs[i].num_allowed_values;
    }
    return card;
}

/*
 * Compute Cartesian strides.
 *
 *   strides[d] = product of num_allowed_values[j] for j > d
 *   strides[last] = 1
 *
 * flat_index = sum(vi[d] * strides[d])   for d in 0..num_label_defs-1
 * where vi[d] is the ordinal of label_value in defs[d].allowed_values.
 */
static void compute_strides(const metric_label_definition_t* defs,
                              uint32_t num_label_defs,
                              size_t*  strides) {
    if (num_label_defs == 0) return;
    strides[num_label_defs - 1] = 1;
    for (int d = (int)num_label_defs - 2; d >= 0; d--) {
        strides[d] = strides[d + 1] * defs[d + 1].num_allowed_values;
    }
}

/*
 * Map a provided label set to its flat array index.
 *
 * Returns -1 if:
 *   - num_labels != m->num_label_defs
 *   - any key is not found in label_defs
 *   - any value is not in the corresponding allowlist
 */
static int compute_flat_index(const metric_t*       m,
                               const metric_label_t* labels,
                               uint32_t              num_labels) {
    /* Unlabelled metric */
    if (m->num_label_defs == 0 && num_labels == 0) return 0;
    if (num_labels != m->num_label_defs)            return -1;

    size_t flat = 0;
    for (uint32_t d = 0; d < m->num_label_defs; d++) {
        const metric_label_definition_t* def = &m->label_defs[d];

        /* Find the label value provided for this dimension's key */
        const char* val = NULL;
        for (uint32_t li = 0; li < num_labels; li++) {
            if (strcmp(labels[li].key, def->key) == 0) {
                val = labels[li].value;
                break;
            }
        }
        if (!val) return -1;

        /* Find the ordinal of val in the allowlist */
        size_t vi = SIZE_MAX;
        for (size_t ai = 0; ai < def->num_allowed_values; ai++) {
            if (def->allowed_values[ai] &&
                strcmp(def->allowed_values[ai], val) == 0) {
                vi = ai;
                break;
            }
        }
        if (vi == SIZE_MAX) return -1;

        flat += vi * m->strides[d];
    }
    return (int)flat;
}

/*
 * Decode flat index fi back into a label set and write into dst[].
 * dst must point to m->num_label_defs metric_label_t entries.
 *
 * Inverse of compute_flat_index: vi[d] = (fi / strides[d]) % nv[d].
 */
static void decode_flat_index(const metric_t* m,
                               size_t          fi,
                               metric_label_t* dst) {
    for (uint32_t d = 0; d < m->num_label_defs; d++) {
        const metric_label_definition_t* def = &m->label_defs[d];
        size_t vi = (fi / m->strides[d]) % def->num_allowed_values;
        strncpy(dst[d].key,   def->key,                    MAX_LABEL_KEY_LEN   - 1);
        strncpy(dst[d].value, def->allowed_values[vi] ? def->allowed_values[vi] : "",
                MAX_LABEL_VALUE_LEN - 1);
        dst[d].key  [MAX_LABEL_KEY_LEN   - 1] = '\0';
        dst[d].value[MAX_LABEL_VALUE_LEN - 1] = '\0';
    }
}

/*
 * Build the labels_table for a metric.
 * Allocates cardinality * num_label_defs metric_label_t entries.
 * Returns NULL on allocation failure (or for unlabelled metrics).
 */
static metric_label_t* build_labels_table(const metric_t* m) {
    if (m->num_label_defs == 0 || m->cardinality == 0) return NULL;

    metric_label_t* tbl = calloc(m->cardinality * m->num_label_defs,
                                  sizeof(metric_label_t));
    if (!tbl) return NULL;

    for (size_t fi = 0; fi < m->cardinality; fi++)
        decode_flat_index(m, fi, tbl + fi * m->num_label_defs);

    return tbl;
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

    size_t max = config ? config->max_metrics : 0;
    if (max == 0 || max > DISTRIC_MAX_METRICS) max = DISTRIC_MAX_METRICS;
    reg->effective_max = max;

    size_t card_cap = config ? config->max_cardinality : 0;
    if (card_cap == 0 || card_cap > DISTRIC_MAX_METRIC_CARDINALITY)
        card_cap = DISTRIC_MAX_METRIC_CARDINALITY;
    reg->effective_cardinality_cap = card_cap;

    atomic_init(&reg->metric_count, 0);
    atomic_init(&reg->state,    (uint32_t)REGISTRY_STATE_MUTABLE);
    atomic_init(&reg->refcount, 1u);

    if (pthread_mutex_init(&reg->register_mutex, NULL) != 0) {
        free(reg);
        return DISTRIC_ERR_INIT_FAILED;
    }

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
    if (prev == 1) metrics_destroy(registry);
}

void metrics_destroy(metrics_registry_t* registry) {
    if (!registry) return;

    atomic_store_explicit(&registry->state,
                          (uint32_t)REGISTRY_STATE_DESTROYED,
                          memory_order_release);

    size_t n = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    for (size_t i = 0; i < n; i++) {
        metric_t* m = &registry->metrics[i];
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) continue;

        /* Free labels table */
        free(m->labels_table);
        m->labels_table = NULL;

        /* Free instance arrays — histograms also need per-instance buckets freed */
        if (m->type == METRIC_TYPE_COUNTER) {
            free(m->instance_array.counter);
            m->instance_array.counter = NULL;
        } else if (m->type == METRIC_TYPE_GAUGE) {
            free(m->instance_array.gauge);
            m->instance_array.gauge = NULL;
        } else { /* METRIC_TYPE_HISTOGRAM */
            if (m->instance_array.histogram) {
                for (size_t fi = 0; fi < m->cardinality; fi++)
                    free(m->instance_array.histogram[fi].buckets);
                free(m->instance_array.histogram);
                m->instance_array.histogram = NULL;
            }
        }

        pthread_mutex_destroy(&m->instance_lock);
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

static distric_err_t register_metric(metrics_registry_t*              registry,
                                      const char*                      name,
                                      const char*                      help,
                                      metric_type_t                    type,
                                      const metric_label_definition_t* label_defs,
                                      size_t                           label_def_count,
                                      metric_t**                       out_metric) {
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

    m->cardinality = card;

    /* Compute strides for O(D) flat-index lookup */
    compute_strides(m->label_defs, m->num_label_defs, m->strides);

    /* Build labels_table: flat-index → label set (used by Prometheus export) */
    m->labels_table = build_labels_table(m);
    if (label_def_count > 0 && !m->labels_table) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    distric_err_t alloc_err = DISTRIC_OK;

    if (type == METRIC_TYPE_COUNTER) {
        /*
         * Allocate one extra slot for labeled metrics.
         * Slots 0..card-1  → labeled combinations (indexed by compute_flat_index).
         * Slot  card        → unlabelled base (written by metrics_counter_inc/add,
         *                      read by metrics_counter_get).
         * For unlabelled metrics (num_label_defs == 0, card == 1) the extra slot
         * is not needed — the single slot IS the unlabelled base.
         */
        size_t alloc_slots = (m->num_label_defs > 0) ? card + 1 : card;
        counter_instance_t* arr = calloc(alloc_slots, sizeof(counter_instance_t));
        if (!arr) { alloc_err = DISTRIC_ERR_ALLOC_FAILURE; goto alloc_fail; }
        for (size_t fi = 0; fi < alloc_slots; fi++)
            atomic_init(&arr[fi].value, 0ULL);
        m->instance_array.counter = arr;

    } else if (type == METRIC_TYPE_GAUGE) {
        size_t alloc_slots = (m->num_label_defs > 0) ? card + 1 : card;
        gauge_instance_t* arr = calloc(alloc_slots, sizeof(gauge_instance_t));
        if (!arr) { alloc_err = DISTRIC_ERR_ALLOC_FAILURE; goto alloc_fail; }
        for (size_t fi = 0; fi < alloc_slots; fi++)
            atomic_init(&arr[fi].value_bits, 0ULL);
        m->instance_array.gauge = arr;

    } else { /* METRIC_TYPE_HISTOGRAM */
        m->num_buckets = HISTOGRAM_BUCKET_COUNT;

        size_t alloc_slots = (m->num_label_defs > 0) ? card + 1 : card;
        histogram_instance_t* arr = calloc(alloc_slots, sizeof(histogram_instance_t));
        if (!arr) { alloc_err = DISTRIC_ERR_ALLOC_FAILURE; goto alloc_fail; }

        for (size_t fi = 0; fi < alloc_slots; fi++) {
            histogram_instance_t* inst = &arr[fi];
            inst->num_buckets = HISTOGRAM_BUCKET_COUNT;
            inst->buckets = calloc(HISTOGRAM_BUCKET_COUNT, sizeof(histogram_bucket_t));
            if (!inst->buckets) {
                /* Free already-allocated bucket arrays for previous instances */
                for (size_t prev = 0; prev < fi; prev++)
                    free(arr[prev].buckets);
                free(arr);
                alloc_err = DISTRIC_ERR_ALLOC_FAILURE;
                goto alloc_fail;
            }
            for (uint32_t b = 0; b < HISTOGRAM_BUCKET_COUNT; b++) {
                inst->buckets[b].upper_bound = HISTOGRAM_DEFAULT_BUCKETS[b];
                atomic_init(&inst->buckets[b].count, 0ULL);
            }
            atomic_init(&inst->count,    0ULL);
            atomic_init(&inst->sum_bits, 0ULL);
        }
        m->instance_array.histogram = arr;
    }

    if (pthread_mutex_init(&m->instance_lock, NULL) != 0) {
        alloc_err = DISTRIC_ERR_INIT_FAILED;
        goto alloc_fail;
    }

    atomic_init(&m->initialized, true);
    atomic_store_explicit(&registry->metric_count, idx + 1, memory_order_release);
    pthread_mutex_unlock(&registry->register_mutex);

    *out_metric = m;
    return DISTRIC_OK;

alloc_fail:
    free(m->labels_table);
    m->labels_table = NULL;
    /* instance_array freed above in the histogram branch; clear the others */
    if (type == METRIC_TYPE_COUNTER) { free(m->instance_array.counter); m->instance_array.counter = NULL; }
    if (type == METRIC_TYPE_GAUGE)   { free(m->instance_array.gauge);   m->instance_array.gauge   = NULL; }
    pthread_mutex_unlock(&registry->register_mutex);
    return alloc_err;
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
 * Counter operations — lock-free, O(D) label lookup
 * ========================================================================= */

void metrics_counter_inc(metric_t* metric) {
    if (!metric || !metric->instance_array.counter) return;
    /*
     * Unlabelled fast path.
     * For labeled metrics: use slot [cardinality] (the reserved unlabelled base).
     * For unlabelled metrics: cardinality == 1, slot [0] is the only slot.
     *
     * This ensures metrics_counter_inc and metrics_counter_inc_labels never
     * write to the same slot, giving true isolation between the two paths.
     */
    size_t idx = (metric->num_label_defs > 0) ? metric->cardinality : 0;
    atomic_fetch_add_explicit(&metric->instance_array.counter[idx].value,
                              1ULL, memory_order_relaxed);
}

void metrics_counter_add(metric_t* metric, uint64_t value) {
    if (!metric || !metric->instance_array.counter) return;
    size_t idx = (metric->num_label_defs > 0) ? metric->cardinality : 0;
    atomic_fetch_add_explicit(&metric->instance_array.counter[idx].value,
                              value, memory_order_relaxed);
}

distric_err_t metrics_counter_inc_labels(metric_t* metric,
                                          const metric_label_t* labels,
                                          uint32_t num_labels) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    int idx = compute_flat_index(metric, labels, num_labels);
    if (idx < 0) return DISTRIC_ERR_INVALID_LABEL;
    atomic_fetch_add_explicit(&metric->instance_array.counter[idx].value,
                              1ULL, memory_order_relaxed);
    return DISTRIC_OK;
}

distric_err_t metrics_counter_add_labels(metric_t* metric,
                                          const metric_label_t* labels,
                                          uint32_t num_labels,
                                          uint64_t value) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    int idx = compute_flat_index(metric, labels, num_labels);
    if (idx < 0) return DISTRIC_ERR_INVALID_LABEL;
    atomic_fetch_add_explicit(&metric->instance_array.counter[idx].value,
                              value, memory_order_relaxed);
    return DISTRIC_OK;
}

uint64_t metrics_counter_get(metric_t* metric) {
    if (!metric || !metric->instance_array.counter) return 0;
    /* Reads the unlabelled base slot — same isolation rule as counter_inc. */
    size_t idx = (metric->num_label_defs > 0) ? metric->cardinality : 0;
    return atomic_load_explicit(&metric->instance_array.counter[idx].value,
                                memory_order_relaxed);
}

uint64_t metrics_counter_get_labeled_total(metric_t* metric) {
    if (!metric || !metric->instance_array.counter) return 0;
    if (metric->num_label_defs == 0 || metric->cardinality == 0) {
        /* Unlabelled metric — single slot at index 0. */
        return atomic_load_explicit(&metric->instance_array.counter[0].value,
                                    memory_order_relaxed);
    }
    /* Sum all labeled slots [0..cardinality-1]. */
    uint64_t total = 0;
    for (size_t i = 0; i < metric->cardinality; i++) {
        total += atomic_load_explicit(&metric->instance_array.counter[i].value,
                                      memory_order_relaxed);
    }
    return total;
}

/* ============================================================================
 * Gauge operations — lock-free, O(D) label lookup
 * ========================================================================= */

void metrics_gauge_set(metric_t* metric, double value) {
    if (!metric || !metric->instance_array.gauge) return;
    atomic_store_explicit(&metric->instance_array.gauge[0].value_bits,
                          double_to_bits(value), memory_order_relaxed);
}

distric_err_t metrics_gauge_set_labels(metric_t* metric,
                                        const metric_label_t* labels,
                                        uint32_t num_labels,
                                        double value) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    int idx = compute_flat_index(metric, labels, num_labels);
    if (idx < 0) return DISTRIC_ERR_INVALID_LABEL;
    atomic_store_explicit(&metric->instance_array.gauge[idx].value_bits,
                          double_to_bits(value), memory_order_relaxed);
    return DISTRIC_OK;
}

double metrics_gauge_get(metric_t* metric) {
    if (!metric || !metric->instance_array.gauge) return 0.0;
    return bits_to_double(
        atomic_load_explicit(&metric->instance_array.gauge[0].value_bits,
                             memory_order_relaxed));
}

/* ============================================================================
 * Histogram operations — lock-free, O(D) label lookup + O(B) bucket scan
 * ========================================================================= */

void metrics_histogram_observe(metric_t* metric, double value) {
    if (!metric || !metric->instance_array.histogram) return;
    histogram_instance_t* inst = &metric->instance_array.histogram[0];

    atomic_fetch_add_explicit(&inst->count, 1ULL, memory_order_relaxed);

    /* Accumulate sum via CAS loop (double stored as uint64 bits) */
    uint64_t old_bits, new_bits;
    do {
        old_bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
        new_bits = double_to_bits(bits_to_double(old_bits) + value);
    } while (!atomic_compare_exchange_weak_explicit(
        &inst->sum_bits, &old_bits, new_bits,
        memory_order_relaxed, memory_order_relaxed));

    for (uint32_t i = 0; i < inst->num_buckets; i++) {
        if (value <= inst->buckets[i].upper_bound)
            atomic_fetch_add_explicit(&inst->buckets[i].count, 1ULL,
                                      memory_order_relaxed);
    }
}

distric_err_t metrics_histogram_observe_labels(metric_t* metric,
                                                const metric_label_t* labels,
                                                uint32_t num_labels,
                                                double value) {
    if (!metric || (!labels && num_labels > 0)) return DISTRIC_ERR_INVALID_ARG;
    int idx = compute_flat_index(metric, labels, num_labels);
    if (idx < 0) return DISTRIC_ERR_INVALID_LABEL;

    histogram_instance_t* inst = &metric->instance_array.histogram[idx];

    atomic_fetch_add_explicit(&inst->count, 1ULL, memory_order_relaxed);

    uint64_t old_bits, new_bits;
    do {
        old_bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
        new_bits = double_to_bits(bits_to_double(old_bits) + value);
    } while (!atomic_compare_exchange_weak_explicit(
        &inst->sum_bits, &old_bits, new_bits,
        memory_order_relaxed, memory_order_relaxed));

    for (uint32_t i = 0; i < inst->num_buckets; i++) {
        if (value <= inst->buckets[i].upper_bound)
            atomic_fetch_add_explicit(&inst->buckets[i].count, 1ULL,
                                      memory_order_relaxed);
    }
    return DISTRIC_OK;
}

uint64_t metrics_histogram_get_count(metric_t* metric) {
    if (!metric || !metric->instance_array.histogram) return 0;
    return atomic_load_explicit(&metric->instance_array.histogram[0].count,
                                memory_order_relaxed);
}

double metrics_histogram_get_sum(metric_t* metric) {
    if (!metric || !metric->instance_array.histogram) return 0.0;
    return bits_to_double(
        atomic_load_explicit(&metric->instance_array.histogram[0].sum_bits,
                             memory_order_relaxed));
}

/* ============================================================================
 * Prometheus export
 *
 * Iterates over all cardinality instances per metric and renders them in
 * the Prometheus text format.  Labels are retrieved from labels_table[fi].
 *
 * Buffer growth: ENSURE_SPACE doubles the buffer whenever space is needed.
 * ========================================================================= */

#define ENSURE_SPACE(needed)                                               \
    while (offset + (size_t)(needed) > buf_size) {                         \
        buf_size *= 2;                                                     \
        char* nb = realloc(buf, buf_size);                                 \
        if (!nb) { free(buf); return DISTRIC_ERR_NO_MEMORY; }             \
        buf = nb;                                                          \
    }

/*
 * Append "{k1="v1",k2="v2"}" to *buf_ptr.
 * No-op for unlabelled instances (num_labels == 0).
 */
static void append_label_set(char**                buf_ptr,
                               size_t*               offset_ptr,
                               size_t*               buf_size_ptr,
                               const metric_label_t* labels,
                               uint32_t              num_labels) {
    if (num_labels == 0) return;

    char*  buf      = *buf_ptr;
    size_t offset   = *offset_ptr;
    size_t buf_size = *buf_size_ptr;

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
}

distric_err_t metrics_export_prometheus(metrics_registry_t* registry,
                                         char**              out_buffer,
                                         size_t*             out_size) {
    if (!registry || !out_buffer || !out_size) return DISTRIC_ERR_INVALID_ARG;

    size_t buf_size = 65536;
    char*  buf = malloc(buf_size);
    if (!buf) return DISTRIC_ERR_NO_MEMORY;
    size_t offset = 0;

    size_t n = atomic_load_explicit(&registry->metric_count, memory_order_acquire);

    for (size_t mi = 0; mi < n; mi++) {
        metric_t* m = &registry->metrics[mi];
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire)) continue;

        /* HELP / TYPE header */
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

        /* Retrieve the labels_table pointer (may be NULL for unlabelled) */
        const metric_label_t* tbl = (const metric_label_t*)m->labels_table;

        /* ------------------------------------------------------------------ */
        if (m->type == METRIC_TYPE_COUNTER) {
            if (!m->instance_array.counter || m->cardinality == 0) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 8);
                w = snprintf(buf + offset, buf_size - offset, "%s 0\n", m->name);
                if (w > 0) offset += (size_t)w;
                continue;
            }
            for (size_t fi = 0; fi < m->cardinality; fi++) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN +
                             MAX_METRIC_LABELS * (MAX_LABEL_KEY_LEN +
                                                   MAX_LABEL_VALUE_LEN + 8) + 32);

                w = snprintf(buf + offset, buf_size - offset, "%s", m->name);
                if (w > 0) offset += (size_t)w;

                const metric_label_t* lset =
                    tbl ? (tbl + fi * m->num_label_defs) : NULL;
                append_label_set(&buf, &offset, &buf_size,
                                  lset, m->num_label_defs);

                uint64_t val = atomic_load_explicit(
                    &m->instance_array.counter[fi].value, memory_order_relaxed);
                w = snprintf(buf + offset, buf_size - offset, " %lu\n",
                             (unsigned long)val);
                if (w > 0) offset += (size_t)w;
            }

        /* ------------------------------------------------------------------ */
        } else if (m->type == METRIC_TYPE_GAUGE) {
            if (!m->instance_array.gauge || m->cardinality == 0) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 8);
                w = snprintf(buf + offset, buf_size - offset, "%s 0\n", m->name);
                if (w > 0) offset += (size_t)w;
                continue;
            }
            for (size_t fi = 0; fi < m->cardinality; fi++) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN +
                             MAX_METRIC_LABELS * (MAX_LABEL_KEY_LEN +
                                                   MAX_LABEL_VALUE_LEN + 8) + 32);

                w = snprintf(buf + offset, buf_size - offset, "%s", m->name);
                if (w > 0) offset += (size_t)w;

                const metric_label_t* lset =
                    tbl ? (tbl + fi * m->num_label_defs) : NULL;
                append_label_set(&buf, &offset, &buf_size,
                                  lset, m->num_label_defs);

                double val = bits_to_double(atomic_load_explicit(
                    &m->instance_array.gauge[fi].value_bits, memory_order_relaxed));
                w = snprintf(buf + offset, buf_size - offset, " %g\n", val);
                if (w > 0) offset += (size_t)w;
            }

        /* ------------------------------------------------------------------ */
        } else { /* HISTOGRAM */
            if (!m->instance_array.histogram || m->cardinality == 0) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 32);
                w = snprintf(buf + offset, buf_size - offset,
                             "%s_count 0\n%s_sum 0\n", m->name, m->name);
                if (w > 0) offset += (size_t)w;
                continue;
            }
            for (size_t fi = 0; fi < m->cardinality; fi++) {
                histogram_instance_t* inst = &m->instance_array.histogram[fi];
                const metric_label_t* lset =
                    tbl ? (tbl + fi * m->num_label_defs) : NULL;

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

                    /*
                     * Histogram label set merges user labels with the
                     * mandatory "le" bucket label.  Build it inline.
                     */
                    if (m->num_label_defs > 0 && lset) {
                        w = snprintf(buf + offset, buf_size - offset, "{");
                        if (w > 0) offset += (size_t)w;
                        for (uint32_t li = 0; li < m->num_label_defs; li++) {
                            w = snprintf(buf + offset, buf_size - offset,
                                         "%s=\"%s\",",
                                         lset[li].key, lset[li].value);
                            if (w > 0) offset += (size_t)w;
                        }
                        w = snprintf(buf + offset, buf_size - offset,
                                     "le=\"%s\"}", ub_str);
                    } else {
                        w = snprintf(buf + offset, buf_size - offset,
                                     "{le=\"%s\"}", ub_str);
                    }
                    if (w > 0) offset += (size_t)w;

                    uint64_t bc = atomic_load_explicit(
                        &inst->buckets[bi].count, memory_order_relaxed);
                    w = snprintf(buf + offset, buf_size - offset, " %lu\n",
                                 (unsigned long)bc);
                    if (w > 0) offset += (size_t)w;
                }

                /* _count and _sum lines */
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 256 + 64);

                uint64_t cnt = atomic_load_explicit(
                    &inst->count, memory_order_relaxed);
                double   sum = bits_to_double(atomic_load_explicit(
                    &inst->sum_bits, memory_order_relaxed));

                w = snprintf(buf + offset, buf_size - offset,
                             "%s_count", m->name);
                if (w > 0) offset += (size_t)w;
                append_label_set(&buf, &offset, &buf_size,
                                  lset, m->num_label_defs);
                w = snprintf(buf + offset, buf_size - offset, " %lu\n",
                             (unsigned long)cnt);
                if (w > 0) offset += (size_t)w;

                w = snprintf(buf + offset, buf_size - offset,
                             "%s_sum", m->name);
                if (w > 0) offset += (size_t)w;
                append_label_set(&buf, &offset, &buf_size,
                                  lset, m->num_label_defs);
                w = snprintf(buf + offset, buf_size - offset, " %g\n", sum);
                if (w > 0) offset += (size_t)w;
            }
        }
    }

    buf[offset] = '\0';
    *out_buffer = buf;
    *out_size   = offset;
    return DISTRIC_OK;
}

#undef ENSURE_SPACE