/*
 * metrics.c — DistriC Observability Library — Metrics Implementation
 *
 * =============================================================================
 * LABEL CARDINALITY — STRICT ENFORCEMENT (Production Blocker #1)
 * =============================================================================
 *
 * ALL label allowlist enforcement is now unconditionally strict.
 *
 *  1. Registration: a label dimension with num_allowed_values == 0 or
 *     allowed_values == NULL is UNBOUNDED.  compute_cardinality() returns 0,
 *     and registration fails with DISTRIC_ERR_HIGH_CARDINALITY immediately.
 *
 *  2. Update-time: validate_label() returns false for NULL/empty allowlists
 *     (defence-in-depth; correctly registered metrics never reach this path).
 *
 *  3. Debug builds only: define DISTRIC_OBS_ALLOW_OPEN_LABELS to restore
 *     pre-production behaviour.  NEVER set this in production.
 *
 *  4. Enforcement at registration time (early) AND update time (defence).
 *
 * Thread-safety:
 *   - metrics_init / metrics_destroy: caller must serialize.
 *   - metrics_register_*: serialised by registry->register_mutex.
 *   - counter_inc / gauge_set / histogram_observe: lock-free atomics.
 *   - metrics_export_prometheus: safe after metrics_freeze().
 */

#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>

/* ============================================================================
 * Internal constants
 * ========================================================================= */

static const double HISTOGRAM_DEFAULT_BUCKETS[] = {
    0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, INFINITY
};
#define HISTOGRAM_BUCKET_COUNT \
    ((int)(sizeof(HISTOGRAM_DEFAULT_BUCKETS)/sizeof(HISTOGRAM_DEFAULT_BUCKETS[0])))

typedef enum {
    REGISTRY_STATE_MUTABLE = 0,
    REGISTRY_STATE_FROZEN  = 1,
} registry_state_t;

/* ============================================================================
 * Internal instance types (lock-free linked lists per metric)
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
    _Atomic uint64_t            value_bits;
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
    _Atomic uint64_t             sum_bits;
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

struct metrics_registry_s {
    metric_t         metrics[MAX_METRICS];
    _Atomic size_t   metric_count;
    _Atomic uint32_t state;
    _Atomic uint32_t refcount;
    pthread_mutex_t  register_mutex;
};

/* ============================================================================
 * Cardinality helpers
 * ========================================================================= */

/*
 * Returns 0 if any dimension is unbounded or the product overflows cap.
 * Returns 1 for an unlabelled metric.
 * STRICT: NULL or zero-size allowlist is always unbounded.
 */
static uint64_t compute_cardinality(const metric_label_definition_t* defs,
                                    size_t count) {
    if (!defs || count == 0) return 1;
    uint64_t c = 1;
    for (size_t i = 0; i < count; i++) {
        if (!defs[i].allowed_values || defs[i].num_allowed_values == 0)
            return 0;
        c *= defs[i].num_allowed_values;
        if (c > (uint64_t)MAX_METRIC_CARDINALITY) return 0;
    }
    return c;
}

static distric_err_t validate_registration_cardinality(
    const metric_label_definition_t* label_defs,
    size_t label_def_count) {

    if (!label_defs || label_def_count == 0) return DISTRIC_OK;
    uint64_t card = compute_cardinality(label_defs, label_def_count);
    if (card == 0 || card > (uint64_t)MAX_METRIC_CARDINALITY)
        return DISTRIC_ERR_HIGH_CARDINALITY;
    return DISTRIC_OK;
}

/*
 * Validate one label key/value against one definition.
 *
 * STRICT (default/production): NULL or empty allowlist returns false.
 * DEBUG (DISTRIC_OBS_ALLOW_OPEN_LABELS): NULL allowlist accepts any value.
 */
static bool validate_label(const metric_label_t* label,
                            const metric_label_definition_t* def) {
    if (strcmp(label->key, def->key) != 0) return false;

#ifdef DISTRIC_OBS_ALLOW_OPEN_LABELS
    if (!def->allowed_values || def->num_allowed_values == 0) return true;
#else
    if (!def->allowed_values || def->num_allowed_values == 0) return false;
#endif

    for (uint32_t i = 0; i < def->num_allowed_values; i++) {
        if (def->allowed_values[i] &&
            strcmp(label->value, def->allowed_values[i]) == 0)
            return true;
    }
    return false;
}

static distric_err_t validate_labels(const metric_label_t* labels,
                                      uint32_t num_labels,
                                      const metric_label_definition_t* defs,
                                      uint32_t num_defs) {
    if (num_labels != num_defs) return DISTRIC_ERR_INVALID_LABEL;
    for (uint32_t i = 0; i < num_labels; i++) {
        bool found = false;
        for (uint32_t j = 0; j < num_defs; j++) {
            if (validate_label(&labels[i], &defs[j])) { found = true; break; }
        }
        if (!found) return DISTRIC_ERR_INVALID_LABEL;
    }
    return DISTRIC_OK;
}

/* ============================================================================
 * Registry lifecycle
 * ========================================================================= */

distric_err_t metrics_init(metrics_registry_t** registry) {
    if (!registry) return DISTRIC_ERR_INVALID_ARG;
    metrics_registry_t* r = calloc(1, sizeof(metrics_registry_t));
    if (!r) return DISTRIC_ERR_ALLOC_FAILURE;
    atomic_init(&r->metric_count, 0);
    atomic_init(&r->state,    REGISTRY_STATE_MUTABLE);
    atomic_init(&r->refcount, 1);
    pthread_mutex_init(&r->register_mutex, NULL);
    *registry = r;
    return DISTRIC_OK;
}

void metrics_retain(metrics_registry_t* r) {
    if (r) atomic_fetch_add_explicit(&r->refcount, 1, memory_order_relaxed);
}

void metrics_release(metrics_registry_t* r) {
    if (!r) return;
    if (atomic_fetch_sub_explicit(&r->refcount, 1, memory_order_acq_rel) != 1)
        return;
    /* free instance lists */
    size_t n = atomic_load(&r->metric_count);
    for (size_t i = 0; i < n; i++) {
        metric_t* m = &r->metrics[i];
        if (m->type == METRIC_TYPE_COUNTER) {
            counter_instance_t* inst =
                atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
            while (inst) {
                counter_instance_t* nx = inst->next;
                free(inst);
                inst = nx;
            }
            pthread_mutex_destroy(&m->data.counter.instance_lock);
        } else if (m->type == METRIC_TYPE_GAUGE) {
            gauge_instance_t* inst =
                atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
            while (inst) {
                gauge_instance_t* nx = inst->next;
                free(inst);
                inst = nx;
            }
            pthread_mutex_destroy(&m->data.gauge.instance_lock);
        } else if (m->type == METRIC_TYPE_HISTOGRAM) {
            histogram_instance_t* inst =
                atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
            while (inst) {
                histogram_instance_t* nx = inst->next;
                free(inst->buckets);
                free(inst);
                inst = nx;
            }
            pthread_mutex_destroy(&m->data.histogram.instance_lock);
        }
    }
    pthread_mutex_destroy(&r->register_mutex);
    free(r);
}

void metrics_destroy(metrics_registry_t* r) { metrics_release(r); }

distric_err_t metrics_freeze(metrics_registry_t* r) {
    if (!r) return DISTRIC_ERR_INVALID_ARG;
    uint32_t prev = atomic_exchange_explicit(&r->state, REGISTRY_STATE_FROZEN,
                                             memory_order_acq_rel);
    return (prev == REGISTRY_STATE_FROZEN) ? DISTRIC_ERR_REGISTRY_FROZEN : DISTRIC_OK;
}

/* ============================================================================
 * Internal registration helper
 * ========================================================================= */

static distric_err_t register_metric(metrics_registry_t* registry,
                                      const char* name,
                                      const char* help,
                                      metric_type_t type,
                                      const metric_label_definition_t* label_defs,
                                      size_t label_def_count,
                                      metric_t** out_metric) {
    if (!registry || !name || !out_metric) return DISTRIC_ERR_INVALID_ARG;
    *out_metric = NULL;

    if (atomic_load_explicit(&registry->state, memory_order_acquire) ==
        REGISTRY_STATE_FROZEN)
        return DISTRIC_ERR_REGISTRY_FROZEN;

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

    /* STRICT CARDINALITY CHECK — primary enforcement point */
    distric_err_t card_err = validate_registration_cardinality(label_defs,
                                                                label_def_count);
    if (card_err != DISTRIC_OK) {
        pthread_mutex_unlock(&registry->register_mutex);
        return card_err;
    }

    metric_t* m = &registry->metrics[idx];
    memset(m, 0, sizeof(*m));
    strncpy(m->name, name, MAX_METRIC_NAME_LEN - 1);
    if (help) strncpy(m->help, help, MAX_METRIC_HELP_LEN - 1);
    m->type = type;
    m->num_label_defs = (uint32_t)(label_def_count > MAX_METRIC_LABELS
                                   ? MAX_METRIC_LABELS : label_def_count);
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

/* ============================================================================
 * Counter operations
 * ========================================================================= */

static counter_instance_t* get_or_create_counter_instance(
    metric_t* m, const metric_label_t* labels, uint32_t num_labels) {

    /* Fast path: search existing. */
    counter_instance_t* inst =
        atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels))
            return inst;
        inst = inst->next;
    }
    /* Slow path: create. */
    pthread_mutex_lock(&m->data.counter.instance_lock);
    inst = atomic_load_explicit(&m->data.counter.instances, memory_order_relaxed);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels)) {
            pthread_mutex_unlock(&m->data.counter.instance_lock);
            return inst;
        }
        inst = inst->next;
    }
    counter_instance_t* ni = calloc(1, sizeof(*ni));
    if (!ni) { pthread_mutex_unlock(&m->data.counter.instance_lock); return NULL; }
    if (labels && num_labels > 0)
        memcpy(ni->labels, labels, num_labels * sizeof(metric_label_t));
    ni->num_labels = num_labels;
    atomic_init(&ni->value, 0);
    ni->next = atomic_load_explicit(&m->data.counter.instances, memory_order_relaxed);
    atomic_store_explicit(&m->data.counter.instances, ni, memory_order_release);
    pthread_mutex_unlock(&m->data.counter.instance_lock);
    return ni;
}

void metrics_counter_inc(metric_t* m) {
    if (!m || m->type != METRIC_TYPE_COUNTER) return;
    counter_instance_t* inst = get_or_create_counter_instance(m, NULL, 0);
    if (inst) atomic_fetch_add_explicit(&inst->value, 1, memory_order_relaxed);
}

void metrics_counter_add(metric_t* m, uint64_t v) {
    if (!m || m->type != METRIC_TYPE_COUNTER) return;
    counter_instance_t* inst = get_or_create_counter_instance(m, NULL, 0);
    if (inst) atomic_fetch_add_explicit(&inst->value, v, memory_order_relaxed);
}

distric_err_t metrics_counter_inc_with_labels(metric_t* m,
    const metric_label_t* labels, size_t label_count, uint64_t value) {
    if (!m || m->type != METRIC_TYPE_COUNTER) return DISTRIC_ERR_INVALID_ARG;
    distric_err_t err = validate_labels(labels, (uint32_t)label_count,
                                        m->label_defs, m->num_label_defs);
    if (err != DISTRIC_OK) return err;
    counter_instance_t* inst = get_or_create_counter_instance(
        m, labels, (uint32_t)label_count);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;
    atomic_fetch_add_explicit(&inst->value, value, memory_order_relaxed);
    return DISTRIC_OK;
}

uint64_t metrics_counter_get(metric_t* m) {
    if (!m || m->type != METRIC_TYPE_COUNTER) return 0;
    counter_instance_t* inst =
        atomic_load_explicit(&m->data.counter.instances, memory_order_acquire);
    uint64_t total = 0;
    while (inst) {
        total += atomic_load_explicit(&inst->value, memory_order_relaxed);
        inst = inst->next;
    }
    return total;
}

/* ============================================================================
 * Gauge operations
 * ========================================================================= */

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
    inst = atomic_load_explicit(&m->data.gauge.instances, memory_order_relaxed);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels)) {
            pthread_mutex_unlock(&m->data.gauge.instance_lock);
            return inst;
        }
        inst = inst->next;
    }
    gauge_instance_t* ni = calloc(1, sizeof(*ni));
    if (!ni) { pthread_mutex_unlock(&m->data.gauge.instance_lock); return NULL; }
    if (labels && num_labels > 0)
        memcpy(ni->labels, labels, num_labels * sizeof(metric_label_t));
    ni->num_labels = num_labels;
    atomic_init(&ni->value_bits, 0);
    ni->next = atomic_load_explicit(&m->data.gauge.instances, memory_order_relaxed);
    atomic_store_explicit(&m->data.gauge.instances, ni, memory_order_release);
    pthread_mutex_unlock(&m->data.gauge.instance_lock);
    return ni;
}

void metrics_gauge_set(metric_t* m, double value) {
    if (!m || m->type != METRIC_TYPE_GAUGE) return;
    gauge_instance_t* inst = get_or_create_gauge_instance(m, NULL, 0);
    if (!inst) return;
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    atomic_store_explicit(&inst->value_bits, bits, memory_order_relaxed);
}

distric_err_t metrics_gauge_set_with_labels(metric_t* m,
    const metric_label_t* labels, size_t label_count, double value) {
    if (!m || m->type != METRIC_TYPE_GAUGE) return DISTRIC_ERR_INVALID_ARG;
    distric_err_t err = validate_labels(labels, (uint32_t)label_count,
                                        m->label_defs, m->num_label_defs);
    if (err != DISTRIC_OK) return err;
    gauge_instance_t* inst = get_or_create_gauge_instance(
        m, labels, (uint32_t)label_count);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    atomic_store_explicit(&inst->value_bits, bits, memory_order_relaxed);
    return DISTRIC_OK;
}

double metrics_gauge_get(metric_t* m) {
    if (!m || m->type != METRIC_TYPE_GAUGE) return 0.0;
    gauge_instance_t* inst =
        atomic_load_explicit(&m->data.gauge.instances, memory_order_acquire);
    if (!inst) return 0.0;
    uint64_t bits = atomic_load_explicit(&inst->value_bits, memory_order_relaxed);
    double v; memcpy(&v, &bits, sizeof(v));
    return v;
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
    inst = atomic_load_explicit(&m->data.histogram.instances, memory_order_relaxed);
    while (inst) {
        if (labels_match(inst->labels, inst->num_labels, labels, num_labels)) {
            pthread_mutex_unlock(&m->data.histogram.instance_lock);
            return inst;
        }
        inst = inst->next;
    }
    histogram_instance_t* ni = calloc(1, sizeof(*ni));
    if (!ni) { pthread_mutex_unlock(&m->data.histogram.instance_lock); return NULL; }
    if (labels && num_labels > 0)
        memcpy(ni->labels, labels, num_labels * sizeof(metric_label_t));
    ni->num_labels = num_labels;
    ni->num_buckets = m->data.histogram.num_buckets;
    ni->buckets = calloc(ni->num_buckets, sizeof(histogram_bucket_t));
    if (!ni->buckets) {
        free(ni);
        pthread_mutex_unlock(&m->data.histogram.instance_lock);
        return NULL;
    }
    for (uint32_t i = 0; i < ni->num_buckets; i++) {
        ni->buckets[i].upper_bound = m->data.histogram.buckets_template[i];
        atomic_init(&ni->buckets[i].count, 0);
    }
    atomic_init(&ni->count, 0);
    atomic_init(&ni->sum_bits, 0);
    ni->next = atomic_load_explicit(&m->data.histogram.instances, memory_order_relaxed);
    atomic_store_explicit(&m->data.histogram.instances, ni, memory_order_release);
    pthread_mutex_unlock(&m->data.histogram.instance_lock);
    return ni;
}

static void histogram_do_observe(histogram_instance_t* inst, double value) {
    for (uint32_t i = 0; i < inst->num_buckets; i++) {
        if (value <= inst->buckets[i].upper_bound)
            atomic_fetch_add_explicit(&inst->buckets[i].count, 1,
                                      memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&inst->count, 1, memory_order_relaxed);
    /* CAS loop for sum (double stored as bits). */
    uint64_t old_bits, new_bits;
    double old_val, new_val;
    do {
        old_bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
        memcpy(&old_val, &old_bits, sizeof(old_val));
        new_val = old_val + value;
        memcpy(&new_bits, &new_val, sizeof(new_bits));
    } while (!atomic_compare_exchange_weak_explicit(
        &inst->sum_bits, &old_bits, new_bits,
        memory_order_relaxed, memory_order_relaxed));
}

void metrics_histogram_observe(metric_t* m, double value) {
    if (!m || m->type != METRIC_TYPE_HISTOGRAM) return;
    histogram_instance_t* inst = get_or_create_histogram_instance(m, NULL, 0);
    if (inst) histogram_do_observe(inst, value);
}

distric_err_t metrics_histogram_observe_with_labels(metric_t* m,
    const metric_label_t* labels, size_t label_count, double value) {
    if (!m || m->type != METRIC_TYPE_HISTOGRAM) return DISTRIC_ERR_INVALID_ARG;
    distric_err_t err = validate_labels(labels, (uint32_t)label_count,
                                        m->label_defs, m->num_label_defs);
    if (err != DISTRIC_OK) return err;
    histogram_instance_t* inst = get_or_create_histogram_instance(
        m, labels, (uint32_t)label_count);
    if (!inst) return DISTRIC_ERR_NO_MEMORY;
    histogram_do_observe(inst, value);
    return DISTRIC_OK;
}

uint64_t metrics_histogram_get_count(metric_t* m) {
    if (!m || m->type != METRIC_TYPE_HISTOGRAM) return 0;
    histogram_instance_t* inst =
        atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
    if (!inst) return 0;
    return atomic_load_explicit(&inst->count, memory_order_relaxed);
}

double metrics_histogram_get_sum(metric_t* m) {
    if (!m || m->type != METRIC_TYPE_HISTOGRAM) return 0.0;
    histogram_instance_t* inst =
        atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
    if (!inst) return 0.0;
    uint64_t bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
    double v; memcpy(&v, &bits, sizeof(v));
    return v;
}

/* ============================================================================
 * Prometheus text-format export
 * ========================================================================= */

#define ENSURE_SPACE(needed)                                              \
    while (offset + (needed) >= buf_size) {                              \
        buf_size *= 2;                                                   \
        char* nb = realloc(buf, buf_size);                               \
        if (!nb) { free(buf); return DISTRIC_ERR_NO_MEMORY; }           \
        buf = nb;                                                        \
    }

distric_err_t metrics_export_prometheus(metrics_registry_t* registry,
                                         char** out_buffer,
                                         size_t* out_size) {
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
                w = snprintf(buf + offset, buf_size - offset,
                             "%s 0\n", m->name);
                if (w > 0) offset += (size_t)w;
            }
            while (inst) {
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + MAX_METRIC_LABELS *
                             (MAX_LABEL_KEY_LEN + MAX_LABEL_VALUE_LEN + 4) + 64);
                w = snprintf(buf + offset, buf_size - offset, "%s", m->name);
                if (w > 0) offset += (size_t)w;
                if (inst->num_labels > 0) {
                    w = snprintf(buf + offset, buf_size - offset, "{");
                    if (w > 0) offset += (size_t)w;
                    for (uint32_t li = 0; li < inst->num_labels; li++) {
                        w = snprintf(buf + offset, buf_size - offset,
                                     "%s%s=\"%s\"",
                                     li ? "," : "",
                                     inst->labels[li].key,
                                     inst->labels[li].value);
                        if (w > 0) offset += (size_t)w;
                    }
                    w = snprintf(buf + offset, buf_size - offset, "}");
                    if (w > 0) offset += (size_t)w;
                }
                uint64_t val = atomic_load_explicit(&inst->value, memory_order_relaxed);
                w = snprintf(buf + offset, buf_size - offset, " %lu\n", (unsigned long)val);
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
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 256);
                w = snprintf(buf + offset, buf_size - offset, "%s", m->name);
                if (w > 0) offset += (size_t)w;
                if (inst->num_labels > 0) {
                    w = snprintf(buf + offset, buf_size - offset, "{");
                    if (w > 0) offset += (size_t)w;
                    for (uint32_t li = 0; li < inst->num_labels; li++) {
                        w = snprintf(buf + offset, buf_size - offset,
                                     "%s%s=\"%s\"",
                                     li ? "," : "",
                                     inst->labels[li].key,
                                     inst->labels[li].value);
                        if (w > 0) offset += (size_t)w;
                    }
                    w = snprintf(buf + offset, buf_size - offset, "}");
                    if (w > 0) offset += (size_t)w;
                }
                uint64_t bits = atomic_load_explicit(&inst->value_bits, memory_order_relaxed);
                double val; memcpy(&val, &bits, sizeof(val));
                w = snprintf(buf + offset, buf_size - offset, " %g\n", val);
                if (w > 0) offset += (size_t)w;
                inst = inst->next;
            }
        } else {
            histogram_instance_t* inst =
                atomic_load_explicit(&m->data.histogram.instances, memory_order_acquire);
            while (inst) {
                for (uint32_t bi = 0; bi < inst->num_buckets; bi++) {
                    ENSURE_SPACE(MAX_METRIC_NAME_LEN + 256);
                    char bound_str[64];
                    if (isinf(inst->buckets[bi].upper_bound))
                        snprintf(bound_str, sizeof(bound_str), "+Inf");
                    else
                        snprintf(bound_str, sizeof(bound_str), "%g",
                                 inst->buckets[bi].upper_bound);
                    w = snprintf(buf + offset, buf_size - offset,
                                 "%s_bucket{le=\"%s\"} %lu\n",
                                 m->name, bound_str,
                                 (unsigned long)atomic_load_explicit(
                                     &inst->buckets[bi].count, memory_order_relaxed));
                    if (w > 0) offset += (size_t)w;
                }
                ENSURE_SPACE(MAX_METRIC_NAME_LEN + 128);
                uint64_t cnt = atomic_load_explicit(&inst->count, memory_order_relaxed);
                uint64_t sbits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
                double sum; memcpy(&sum, &sbits, sizeof(sum));
                w = snprintf(buf + offset, buf_size - offset,
                             "%s_sum %g\n%s_count %lu\n",
                             m->name, sum, m->name, (unsigned long)cnt);
                if (w > 0) offset += (size_t)w;
                inst = inst->next;
            }
        }
    }

    ENSURE_SPACE(1);
    buf[offset] = '\0';
    *out_buffer = buf;
    *out_size   = offset;
    return DISTRIC_OK;
}