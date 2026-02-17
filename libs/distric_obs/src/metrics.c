/*
 * metrics.c — DistriC Observability Library — Metrics Implementation
 *
 * =============================================================================
 * LABEL CARDINALITY — STRICT ENFORCEMENT (Production Blocker #1)
 * =============================================================================
 *
 * ALL label allowlist enforcement is now unconditionally strict.  There is no
 * runtime opt-out and no "permit any value" escape hatch.  Specifically:
 *
 *  1. Registration: a label dimension with num_allowed_values == 0 (or
 *     allowed_values == NULL) is treated as UNBOUNDED.  compute_cardinality()
 *     returns 0 for any unbounded dimension, and the registration function
 *     rejects it with DISTRIC_ERR_HIGH_CARDINALITY.  The public header comment
 *     that previously described allowed_values=NULL as a valid "low-cardinality"
 *     escape hatch has been removed.
 *
 *  2. Update-time validation: validate_label() no longer silently accepts a
 *     label whose dimension has a NULL or empty allowlist.  In the strict
 *     (production) code path it returns false immediately, which translates to
 *     DISTRIC_ERR_INVALID_LABEL for the caller.  This path should be
 *     unreachable in a correctly-registered metric, but the check is present as
 *     a defence-in-depth layer.
 *
 *  3. Debug / test builds: define DISTRIC_OBS_ALLOW_OPEN_LABELS at compile time
 *     (e.g. -DDISTRIC_OBS_ALLOW_OPEN_LABELS) to restore the pre-production
 *     behaviour where a NULL allowlist accepts any label value.  This flag must
 *     NEVER be set in production builds.
 *
 *  4. Enforcement happens at registration time (pre-condition on metric
 *     definition) AND at update time (defence-in-depth).  Any metric that
 *     passed registration is guaranteed to have a fully-enumerated allowlist.
 *
 * Thread-safety model:
 *   - metrics_init / metrics_destroy: external serialisation required.
 *   - metrics_register_*: serialised internally via registry->register_mutex.
 *   - metrics_counter_inc / metrics_gauge_set / metrics_histogram_observe:
 *     lock-free atomic operations — safe from any thread.
 *   - metrics_export_prometheus: safe to call concurrently with update
 *     operations after the registry is frozen.
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

const double HISTOGRAM_BUCKETS[10] = {
    0.001, 0.01, 0.1, 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, INFINITY
};

#define MAX_METRICS            1024
#define MAX_METRIC_NAME_LEN    128
#define MAX_METRIC_HELP_LEN    256
#define HISTOGRAM_BUCKET_COUNT 10

typedef enum {
    REGISTRY_STATE_MUTABLE = 0,
    REGISTRY_STATE_FROZEN  = 1,
} registry_state_t;

/* ============================================================================
 * Internal instance types (lock-free linked lists per metric)
 * ========================================================================= */

typedef struct counter_instance_s {
    metric_label_t          labels[MAX_METRIC_LABELS];
    uint32_t                num_labels;
    _Atomic uint64_t        value;
    struct counter_instance_s* next;
} counter_instance_t;

typedef struct gauge_instance_s {
    metric_label_t          labels[MAX_METRIC_LABELS];
    uint32_t                num_labels;
    _Atomic uint64_t        value_bits;   /* double stored as bit-identical uint64 */
    struct gauge_instance_s* next;
} gauge_instance_t;

typedef struct histogram_bucket_s {
    double           upper_bound;
    _Atomic uint64_t count;
} histogram_bucket_t;

typedef struct histogram_instance_s {
    metric_label_t          labels[MAX_METRIC_LABELS];
    uint32_t                num_labels;
    histogram_bucket_t*     buckets;
    uint32_t                num_buckets;
    _Atomic uint64_t        count;
    _Atomic uint64_t        sum_bits;
    struct histogram_instance_s* next;
} histogram_instance_t;

/* ============================================================================
 * metric_t and metrics_registry_t internal layout
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
            histogram_bucket_t             buckets_template[HISTOGRAM_BUCKET_COUNT];
            uint32_t                       num_buckets;
        } histogram;
    } data;
};

struct metrics_registry_s {
    metric_t       metrics[MAX_METRICS];
    _Atomic size_t metric_count;
    _Atomic uint32_t state;
    _Atomic uint32_t refcount;
    pthread_mutex_t  register_mutex;
};

/* ============================================================================
 * Cardinality helpers
 * ========================================================================= */

/*
 * Compute the theoretical number of distinct label-value combinations for a
 * metric definition.
 *
 * Returns 0 if any dimension is unbounded (num_allowed_values == 0 or
 * allowed_values == NULL) — which the caller must treat as "exceeds cap".
 * Returns 0 on overflow (product > MAX_METRIC_CARDINALITY).
 * Returns 1 for a metric with no label dimensions (unlabelled metric).
 *
 * STRICT ENFORCEMENT: a NULL or zero-length allowlist is now considered
 * unbounded regardless of build flags.  There is no escape hatch at
 * registration time.
 */
static uint64_t compute_cardinality(const metric_label_definition_t* defs,
                                    size_t count) {
    if (!defs || count == 0) return 1; /* unlabelled → 1 combination, always valid */
    uint64_t c = 1;
    for (size_t i = 0; i < count; i++) {
        /* Any dimension without a fully-enumerated allowlist is unbounded. */
        if (!defs[i].allowed_values || defs[i].num_allowed_values == 0) {
            return 0; /* unbounded — registration must be rejected */
        }
        c *= defs[i].num_allowed_values;
        if (c > (uint64_t)MAX_METRIC_CARDINALITY) return 0;
    }
    return c;
}

/*
 * Validate a single label key/value pair against one dimension definition.
 *
 * STRICT MODE (default, production):
 *   - Returns false when def->allowed_values is NULL or empty, regardless of
 *     the label value.  This is defence-in-depth: a correctly-registered metric
 *     will never have an empty allowlist (compute_cardinality rejects it at
 *     registration time), but if somehow reached this function will not silently
 *     pass arbitrary values through.
 *
 * DEBUG MODE (define DISTRIC_OBS_ALLOW_OPEN_LABELS):
 *   - Restores pre-production behaviour: NULL allowlist accepts any value.
 *   - MUST NOT be defined in production builds.
 */
static bool validate_label(const metric_label_t*            label,
                            const metric_label_definition_t* def) {
    if (strcmp(label->key, def->key) != 0) {
        return false;
    }

#ifdef DISTRIC_OBS_ALLOW_OPEN_LABELS
    /* Debug / test only: accept any value when allowlist is not specified. */
    if (!def->allowed_values || def->num_allowed_values == 0) {
        return true;
    }
#else
    /*
     * STRICT (production default): an empty allowlist means this dimension
     * was incorrectly registered — reject unconditionally.  Well-formed
     * metrics will always have a fully-enumerated allowlist at this point.
     */
    if (!def->allowed_values || def->num_allowed_values == 0) {
        return false;
    }
#endif

    for (uint32_t i = 0; i < def->num_allowed_values; i++) {
        if (def->allowed_values[i] &&
            strcmp(label->value, def->allowed_values[i]) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Validate an entire label set against the metric's registered definitions.
 *
 * Rules:
 *   - num_labels must equal num_defs (count mismatch → DISTRIC_ERR_INVALID_LABEL).
 *   - Each label must match exactly one definition (key match) and the value
 *     must be in that definition's allowlist.
 *   - Metrics registered with no label_defs accept only unlabelled updates
 *     (num_labels must be 0).
 */
static distric_err_t validate_labels(const metric_label_t*            labels,
                                      uint32_t                         num_labels,
                                      const metric_label_definition_t* defs,
                                      uint32_t                         num_defs) {
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

void metrics_retain(metrics_registry_t* registry) {
    if (registry)
        atomic_fetch_add_explicit(&registry->refcount, 1, memory_order_relaxed);
}

void metrics_release(metrics_registry_t* registry) {
    if (!registry) return;
    uint32_t prev = atomic_fetch_sub_explicit(&registry->refcount, 1,
                                              memory_order_acq_rel);
    if (prev != 1) return;

    /* Free per-metric instance lists. */
    size_t n = atomic_load_explicit(&registry->metric_count, memory_order_acquire);
    for (size_t i = 0; i < n; i++) {
        metric_t* m = &registry->metrics[i];
        switch (m->type) {
        case METRIC_TYPE_COUNTER: {
            counter_instance_t* inst =
                atomic_load_explicit(&m->data.counter.instances,
                                     memory_order_acquire);
            while (inst) {
                counter_instance_t* nxt = inst->next;
                free(inst);
                inst = nxt;
            }
            pthread_mutex_destroy(&m->data.counter.instance_lock);
            break;
        }
        case METRIC_TYPE_GAUGE: {
            gauge_instance_t* inst =
                atomic_load_explicit(&m->data.gauge.instances,
                                     memory_order_acquire);
            while (inst) {
                gauge_instance_t* nxt = inst->next;
                free(inst);
                inst = nxt;
            }
            pthread_mutex_destroy(&m->data.gauge.instance_lock);
            break;
        }
        case METRIC_TYPE_HISTOGRAM: {
            histogram_instance_t* inst =
                atomic_load_explicit(&m->data.histogram.instances,
                                     memory_order_acquire);
            while (inst) {
                histogram_instance_t* nxt = inst->next;
                free(inst->buckets);
                free(inst);
                inst = nxt;
            }
            pthread_mutex_destroy(&m->data.histogram.instance_lock);
            break;
        }
        }
    }

    pthread_mutex_destroy(&registry->register_mutex);
    free(registry);
}

void metrics_destroy(metrics_registry_t* registry) {
    metrics_release(registry);
}

distric_err_t metrics_freeze(metrics_registry_t* registry) {
    if (!registry) return DISTRIC_ERR_INVALID_ARG;
    uint32_t expected = REGISTRY_STATE_MUTABLE;
    if (!atomic_compare_exchange_strong_explicit(
            &registry->state, &expected, REGISTRY_STATE_FROZEN,
            memory_order_release, memory_order_relaxed)) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }
    return DISTRIC_OK;
}

/* ============================================================================
 * Shared registration helper (counter / gauge share the same prologue)
 * ========================================================================= */

/*
 * validate_registration_cardinality:
 *
 * Called inside the register_mutex.  Rejects any registration that would
 * introduce an unbounded label dimension or exceed MAX_METRIC_CARDINALITY.
 *
 * This is the PRIMARY enforcement point.  It runs before the metric is
 * committed to the registry, so a faulty definition can never be queried or
 * updated.
 */
static distric_err_t validate_registration_cardinality(
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count) {
    if (!label_defs || label_def_count == 0) {
        return DISTRIC_OK; /* unlabelled metric — always valid */
    }
    uint64_t card = compute_cardinality(label_defs, label_def_count);
    if (card == 0 || card > (uint64_t)MAX_METRIC_CARDINALITY) {
        return DISTRIC_ERR_HIGH_CARDINALITY;
    }
    return DISTRIC_OK;
}

/* ============================================================================
 * Counter registration & operations
 * ========================================================================= */

distric_err_t metrics_register_counter(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric) {

    if (!registry || !name || !out_metric) return DISTRIC_ERR_INVALID_ARG;
    *out_metric = NULL;

    if (atomic_load_explicit(&registry->state, memory_order_acquire) ==
        REGISTRY_STATE_FROZEN) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }

    pthread_mutex_lock(&registry->register_mutex);

    /* Duplicate name check */
    for (size_t i = 0; i < atomic_load(&registry->metric_count); i++) {
        if (strcmp(registry->metrics[i].name, name) == 0) {
            pthread_mutex_unlock(&registry->register_mutex);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }

    size_t idx = atomic_load_explicit(&registry->metric_count,
                                      memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }

    /*
     * STRICT CARDINALITY CHECK — happens at registration, not at emit time.
     * A registration that violates the allowlist contract is rejected
     * deterministically here, making failures immediate and visible.
     */
    distric_err_t card_err = validate_registration_cardinality(label_defs,
                                                               label_def_count);
    if (card_err != DISTRIC_OK) {
        pthread_mutex_unlock(&registry->register_mutex);
        return card_err;
    }

    metric_t* metric = &registry->metrics[idx];
    memset(metric, 0, sizeof(*metric));

    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    if (help) strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
    metric->type = METRIC_TYPE_COUNTER;
    metric->num_label_defs = (uint32_t)(label_def_count > MAX_METRIC_LABELS
                                        ? MAX_METRIC_LABELS : label_def_count);

    if (label_defs && label_def_count > 0) {
        memcpy(metric->label_defs, label_defs,
               metric->num_label_defs * sizeof(metric_label_definition_t));
    }

    atomic_init(&metric->data.counter.instances, NULL);
    pthread_mutex_init(&metric->data.counter.instance_lock, NULL);
    atomic_init(&metric->initialized, true);
    atomic_store_explicit(&registry->metric_count, idx + 1,
                          memory_order_release);
    pthread_mutex_unlock(&registry->register_mutex);

    *out_metric = metric;
    return DISTRIC_OK;
}

void metrics_counter_inc(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_COUNTER) return;
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances,
                             memory_order_acquire);
    /* Fast-path: unlabelled counter (single instance at head). */
    if (inst && inst->num_labels == 0) {
        atomic_fetch_add_explicit(&inst->value, 1, memory_order_relaxed);
        return;
    }
    /* Slow-path: create unlabelled instance on first use. */
    pthread_mutex_lock(&metric->data.counter.instance_lock);
    inst = atomic_load_explicit(&metric->data.counter.instances,
                                memory_order_relaxed);
    if (!inst || inst->num_labels != 0) {
        counter_instance_t* new_inst = calloc(1, sizeof(*new_inst));
        if (new_inst) {
            new_inst->num_labels = 0;
            atomic_init(&new_inst->value, 0);
            new_inst->next = inst;
            atomic_store_explicit(&metric->data.counter.instances, new_inst,
                                  memory_order_release);
            inst = new_inst;
        }
    }
    pthread_mutex_unlock(&metric->data.counter.instance_lock);
    if (inst) atomic_fetch_add_explicit(&inst->value, 1, memory_order_relaxed);
}

void metrics_counter_add(metric_t* metric, uint64_t value) {
    if (!metric || metric->type != METRIC_TYPE_COUNTER) return;
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances,
                             memory_order_acquire);
    if (inst && inst->num_labels == 0) {
        atomic_fetch_add_explicit(&inst->value, value, memory_order_relaxed);
        return;
    }
    pthread_mutex_lock(&metric->data.counter.instance_lock);
    inst = atomic_load_explicit(&metric->data.counter.instances,
                                memory_order_relaxed);
    if (!inst || inst->num_labels != 0) {
        counter_instance_t* new_inst = calloc(1, sizeof(*new_inst));
        if (new_inst) {
            new_inst->num_labels = 0;
            atomic_init(&new_inst->value, 0);
            new_inst->next = inst;
            atomic_store_explicit(&metric->data.counter.instances, new_inst,
                                  memory_order_release);
            inst = new_inst;
        }
    }
    pthread_mutex_unlock(&metric->data.counter.instance_lock);
    if (inst) atomic_fetch_add_explicit(&inst->value, value, memory_order_relaxed);
}

distric_err_t metrics_counter_inc_with_labels(
    metric_t*              metric,
    const metric_label_t*  labels,
    size_t                 label_count,
    uint64_t               value) {

    if (!metric || metric->type != METRIC_TYPE_COUNTER) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    distric_err_t err = validate_labels(labels, (uint32_t)label_count,
                                        metric->label_defs,
                                        metric->num_label_defs);
    if (err != DISTRIC_OK) return err;

    /* Fast-path: search existing instances. */
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances,
                             memory_order_acquire);
    while (inst) {
        if (inst->num_labels == (uint32_t)label_count) {
            bool match = true;
            for (uint32_t i = 0; i < inst->num_labels && match; i++) {
                if (strcmp(inst->labels[i].key,   labels[i].key)   != 0 ||
                    strcmp(inst->labels[i].value, labels[i].value) != 0) {
                    match = false;
                }
            }
            if (match) {
                atomic_fetch_add_explicit(&inst->value, value,
                                          memory_order_relaxed);
                return DISTRIC_OK;
            }
        }
        inst = inst->next;
    }

    /* Slow-path: create a new labelled instance. */
    pthread_mutex_lock(&metric->data.counter.instance_lock);

    /* Re-check after acquiring the lock (TOCTOU). */
    inst = atomic_load_explicit(&metric->data.counter.instances,
                                memory_order_relaxed);
    while (inst) {
        if (inst->num_labels == (uint32_t)label_count) {
            bool match = true;
            for (uint32_t i = 0; i < inst->num_labels && match; i++) {
                if (strcmp(inst->labels[i].key,   labels[i].key)   != 0 ||
                    strcmp(inst->labels[i].value, labels[i].value) != 0) {
                    match = false;
                }
            }
            if (match) {
                atomic_fetch_add_explicit(&inst->value, value,
                                          memory_order_relaxed);
                pthread_mutex_unlock(&metric->data.counter.instance_lock);
                return DISTRIC_OK;
            }
        }
        inst = inst->next;
    }

    counter_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&metric->data.counter.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    memcpy(new_inst->labels, labels, label_count * sizeof(metric_label_t));
    new_inst->num_labels = (uint32_t)label_count;
    atomic_init(&new_inst->value, value);
    new_inst->next = atomic_load_explicit(&metric->data.counter.instances,
                                          memory_order_relaxed);
    atomic_store_explicit(&metric->data.counter.instances, new_inst,
                          memory_order_release);

    pthread_mutex_unlock(&metric->data.counter.instance_lock);
    return DISTRIC_OK;
}

/* ============================================================================
 * Gauge registration & operations
 * ========================================================================= */

distric_err_t metrics_register_gauge(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric) {

    if (!registry || !name || !out_metric) return DISTRIC_ERR_INVALID_ARG;
    *out_metric = NULL;

    if (atomic_load_explicit(&registry->state, memory_order_acquire) ==
        REGISTRY_STATE_FROZEN) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }

    pthread_mutex_lock(&registry->register_mutex);

    for (size_t i = 0; i < atomic_load(&registry->metric_count); i++) {
        if (strcmp(registry->metrics[i].name, name) == 0) {
            pthread_mutex_unlock(&registry->register_mutex);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }

    size_t idx = atomic_load_explicit(&registry->metric_count,
                                      memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }

    distric_err_t card_err = validate_registration_cardinality(label_defs,
                                                               label_def_count);
    if (card_err != DISTRIC_OK) {
        pthread_mutex_unlock(&registry->register_mutex);
        return card_err;
    }

    metric_t* metric = &registry->metrics[idx];
    memset(metric, 0, sizeof(*metric));

    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    if (help) strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
    metric->type = METRIC_TYPE_GAUGE;
    metric->num_label_defs = (uint32_t)(label_def_count > MAX_METRIC_LABELS
                                        ? MAX_METRIC_LABELS : label_def_count);
    if (label_defs && label_def_count > 0) {
        memcpy(metric->label_defs, label_defs,
               metric->num_label_defs * sizeof(metric_label_definition_t));
    }

    atomic_init(&metric->data.gauge.instances, NULL);
    pthread_mutex_init(&metric->data.gauge.instance_lock, NULL);
    atomic_init(&metric->initialized, true);
    atomic_store_explicit(&registry->metric_count, idx + 1,
                          memory_order_release);
    pthread_mutex_unlock(&registry->register_mutex);

    *out_metric = metric;
    return DISTRIC_OK;
}

void metrics_gauge_set(metric_t* metric, double value) {
    if (!metric || metric->type != METRIC_TYPE_GAUGE) return;

    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));

    gauge_instance_t* inst =
        atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    if (inst && inst->num_labels == 0) {
        atomic_store_explicit(&inst->value_bits, bits, memory_order_relaxed);
        return;
    }

    pthread_mutex_lock(&metric->data.gauge.instance_lock);
    inst = atomic_load_explicit(&metric->data.gauge.instances,
                                memory_order_relaxed);
    if (!inst || inst->num_labels != 0) {
        gauge_instance_t* new_inst = calloc(1, sizeof(*new_inst));
        if (new_inst) {
            new_inst->num_labels = 0;
            atomic_init(&new_inst->value_bits, bits);
            new_inst->next = inst;
            atomic_store_explicit(&metric->data.gauge.instances, new_inst,
                                  memory_order_release);
            pthread_mutex_unlock(&metric->data.gauge.instance_lock);
            return;
        }
    } else {
        atomic_store_explicit(&inst->value_bits, bits, memory_order_relaxed);
    }
    pthread_mutex_unlock(&metric->data.gauge.instance_lock);
}

distric_err_t metrics_gauge_set_with_labels(
    metric_t*             metric,
    const metric_label_t* labels,
    size_t                label_count,
    double                value) {

    if (!metric || metric->type != METRIC_TYPE_GAUGE) return DISTRIC_ERR_INVALID_ARG;

    distric_err_t err = validate_labels(labels, (uint32_t)label_count,
                                        metric->label_defs,
                                        metric->num_label_defs);
    if (err != DISTRIC_OK) return err;

    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));

    gauge_instance_t* inst =
        atomic_load_explicit(&metric->data.gauge.instances, memory_order_acquire);
    while (inst) {
        if (inst->num_labels == (uint32_t)label_count) {
            bool match = true;
            for (uint32_t i = 0; i < inst->num_labels && match; i++) {
                if (strcmp(inst->labels[i].key,   labels[i].key)   != 0 ||
                    strcmp(inst->labels[i].value, labels[i].value) != 0) {
                    match = false;
                }
            }
            if (match) {
                atomic_store_explicit(&inst->value_bits, bits,
                                      memory_order_relaxed);
                return DISTRIC_OK;
            }
        }
        inst = inst->next;
    }

    pthread_mutex_lock(&metric->data.gauge.instance_lock);
    inst = atomic_load_explicit(&metric->data.gauge.instances,
                                memory_order_relaxed);
    while (inst) {
        if (inst->num_labels == (uint32_t)label_count) {
            bool match = true;
            for (uint32_t i = 0; i < inst->num_labels && match; i++) {
                if (strcmp(inst->labels[i].key,   labels[i].key)   != 0 ||
                    strcmp(inst->labels[i].value, labels[i].value) != 0) {
                    match = false;
                }
            }
            if (match) {
                atomic_store_explicit(&inst->value_bits, bits,
                                      memory_order_relaxed);
                pthread_mutex_unlock(&metric->data.gauge.instance_lock);
                return DISTRIC_OK;
            }
        }
        inst = inst->next;
    }

    gauge_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&metric->data.gauge.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    memcpy(new_inst->labels, labels, label_count * sizeof(metric_label_t));
    new_inst->num_labels = (uint32_t)label_count;
    atomic_init(&new_inst->value_bits, bits);
    new_inst->next = atomic_load_explicit(&metric->data.gauge.instances,
                                          memory_order_relaxed);
    atomic_store_explicit(&metric->data.gauge.instances, new_inst,
                          memory_order_release);

    pthread_mutex_unlock(&metric->data.gauge.instance_lock);
    return DISTRIC_OK;
}

/* ============================================================================
 * Histogram registration & operations
 * ========================================================================= */

distric_err_t metrics_register_histogram(
    metrics_registry_t*              registry,
    const char*                      name,
    const char*                      help,
    const metric_label_definition_t* label_defs,
    size_t                           label_def_count,
    metric_t**                       out_metric) {

    if (!registry || !name || !out_metric) return DISTRIC_ERR_INVALID_ARG;
    *out_metric = NULL;

    if (atomic_load_explicit(&registry->state, memory_order_acquire) ==
        REGISTRY_STATE_FROZEN) {
        return DISTRIC_ERR_REGISTRY_FROZEN;
    }

    pthread_mutex_lock(&registry->register_mutex);

    for (size_t i = 0; i < atomic_load(&registry->metric_count); i++) {
        if (strcmp(registry->metrics[i].name, name) == 0) {
            pthread_mutex_unlock(&registry->register_mutex);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }

    size_t idx = atomic_load_explicit(&registry->metric_count,
                                      memory_order_acquire);
    if (idx >= MAX_METRICS) {
        pthread_mutex_unlock(&registry->register_mutex);
        return DISTRIC_ERR_REGISTRY_FULL;
    }

    distric_err_t card_err = validate_registration_cardinality(label_defs,
                                                               label_def_count);
    if (card_err != DISTRIC_OK) {
        pthread_mutex_unlock(&registry->register_mutex);
        return card_err;
    }

    metric_t* metric = &registry->metrics[idx];
    memset(metric, 0, sizeof(*metric));

    strncpy(metric->name, name, MAX_METRIC_NAME_LEN - 1);
    if (help) strncpy(metric->help, help, MAX_METRIC_HELP_LEN - 1);
    metric->type = METRIC_TYPE_HISTOGRAM;
    metric->num_label_defs = (uint32_t)(label_def_count > MAX_METRIC_LABELS
                                        ? MAX_METRIC_LABELS : label_def_count);
    if (label_defs && label_def_count > 0) {
        memcpy(metric->label_defs, label_defs,
               metric->num_label_defs * sizeof(metric_label_definition_t));
    }

    /* Populate the bucket template (shared upper bounds, per-instance counts). */
    metric->data.histogram.num_buckets = HISTOGRAM_BUCKET_COUNT;
    for (uint32_t i = 0; i < HISTOGRAM_BUCKET_COUNT; i++) {
        metric->data.histogram.buckets_template[i].upper_bound =
            HISTOGRAM_BUCKETS[i];
        atomic_init(&metric->data.histogram.buckets_template[i].count, 0);
    }

    atomic_init(&metric->data.histogram.instances, NULL);
    pthread_mutex_init(&metric->data.histogram.instance_lock, NULL);
    atomic_init(&metric->initialized, true);
    atomic_store_explicit(&registry->metric_count, idx + 1,
                          memory_order_release);
    pthread_mutex_unlock(&registry->register_mutex);

    *out_metric = metric;
    return DISTRIC_OK;
}

void metrics_histogram_observe(metric_t* metric, double value) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) return;

    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances,
                             memory_order_acquire);
    if (!inst || inst->num_labels != 0) {
        pthread_mutex_lock(&metric->data.histogram.instance_lock);
        inst = atomic_load_explicit(&metric->data.histogram.instances,
                                    memory_order_relaxed);
        if (!inst || inst->num_labels != 0) {
            histogram_instance_t* new_inst = calloc(1, sizeof(*new_inst));
            if (!new_inst) {
                pthread_mutex_unlock(&metric->data.histogram.instance_lock);
                return;
            }
            new_inst->num_labels = 0;
            new_inst->num_buckets = metric->data.histogram.num_buckets;
            new_inst->buckets = calloc(new_inst->num_buckets,
                                       sizeof(histogram_bucket_t));
            if (!new_inst->buckets) {
                free(new_inst);
                pthread_mutex_unlock(&metric->data.histogram.instance_lock);
                return;
            }
            for (uint32_t i = 0; i < new_inst->num_buckets; i++) {
                new_inst->buckets[i].upper_bound =
                    metric->data.histogram.buckets_template[i].upper_bound;
                atomic_init(&new_inst->buckets[i].count, 0);
            }
            atomic_init(&new_inst->count, 0);
            atomic_init(&new_inst->sum_bits, 0);
            new_inst->next = inst;
            atomic_store_explicit(&metric->data.histogram.instances, new_inst,
                                  memory_order_release);
            inst = new_inst;
        }
        pthread_mutex_unlock(&metric->data.histogram.instance_lock);
    }

    if (!inst) return;

    atomic_fetch_add_explicit(&inst->count, 1, memory_order_relaxed);

    uint64_t sum_bits;
    memcpy(&sum_bits, &value, sizeof(sum_bits));
    atomic_fetch_add_explicit(&inst->sum_bits, sum_bits, memory_order_relaxed);

    for (uint32_t i = 0; i < inst->num_buckets; i++) {
        if (value <= inst->buckets[i].upper_bound) {
            atomic_fetch_add_explicit(&inst->buckets[i].count, 1,
                                      memory_order_relaxed);
            break;
        }
    }
}

distric_err_t metrics_histogram_observe_with_labels(
    metric_t*             metric,
    const metric_label_t* labels,
    size_t                label_count,
    double                value) {

    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    distric_err_t err = validate_labels(labels, (uint32_t)label_count,
                                        metric->label_defs,
                                        metric->num_label_defs);
    if (err != DISTRIC_OK) return err;

    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances,
                             memory_order_acquire);
    while (inst) {
        if (inst->num_labels == (uint32_t)label_count) {
            bool match = true;
            for (uint32_t i = 0; i < inst->num_labels && match; i++) {
                if (strcmp(inst->labels[i].key,   labels[i].key)   != 0 ||
                    strcmp(inst->labels[i].value, labels[i].value) != 0) {
                    match = false;
                }
            }
            if (match) {
                atomic_fetch_add_explicit(&inst->count, 1, memory_order_relaxed);
                uint64_t bits;
                memcpy(&bits, &value, sizeof(bits));
                atomic_fetch_add_explicit(&inst->sum_bits, bits,
                                          memory_order_relaxed);
                for (uint32_t i = 0; i < inst->num_buckets; i++) {
                    if (value <= inst->buckets[i].upper_bound) {
                        atomic_fetch_add_explicit(&inst->buckets[i].count, 1,
                                                  memory_order_relaxed);
                        break;
                    }
                }
                return DISTRIC_OK;
            }
        }
        inst = inst->next;
    }

    pthread_mutex_lock(&metric->data.histogram.instance_lock);

    /* Re-check under lock. */
    inst = atomic_load_explicit(&metric->data.histogram.instances,
                                memory_order_relaxed);
    while (inst) {
        if (inst->num_labels == (uint32_t)label_count) {
            bool match = true;
            for (uint32_t i = 0; i < inst->num_labels && match; i++) {
                if (strcmp(inst->labels[i].key,   labels[i].key)   != 0 ||
                    strcmp(inst->labels[i].value, labels[i].value) != 0) {
                    match = false;
                }
            }
            if (match) {
                atomic_fetch_add_explicit(&inst->count, 1, memory_order_relaxed);
                uint64_t bits;
                memcpy(&bits, &value, sizeof(bits));
                atomic_fetch_add_explicit(&inst->sum_bits, bits,
                                          memory_order_relaxed);
                for (uint32_t i = 0; i < inst->num_buckets; i++) {
                    if (value <= inst->buckets[i].upper_bound) {
                        atomic_fetch_add_explicit(&inst->buckets[i].count, 1,
                                                  memory_order_relaxed);
                        break;
                    }
                }
                pthread_mutex_unlock(&metric->data.histogram.instance_lock);
                return DISTRIC_OK;
            }
        }
        inst = inst->next;
    }

    histogram_instance_t* new_inst = calloc(1, sizeof(*new_inst));
    if (!new_inst) {
        pthread_mutex_unlock(&metric->data.histogram.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    memcpy(new_inst->labels, labels, label_count * sizeof(metric_label_t));
    new_inst->num_labels = (uint32_t)label_count;
    new_inst->num_buckets = metric->data.histogram.num_buckets;
    new_inst->buckets = calloc(new_inst->num_buckets, sizeof(histogram_bucket_t));
    if (!new_inst->buckets) {
        free(new_inst);
        pthread_mutex_unlock(&metric->data.histogram.instance_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < new_inst->num_buckets; i++) {
        new_inst->buckets[i].upper_bound =
            metric->data.histogram.buckets_template[i].upper_bound;
        atomic_init(&new_inst->buckets[i].count, 0);
    }
    atomic_init(&new_inst->count, 0);
    atomic_init(&new_inst->sum_bits, 0);

    /* Update first, then push to the list. */
    atomic_fetch_add_explicit(&new_inst->count, 1, memory_order_relaxed);
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    atomic_fetch_add_explicit(&new_inst->sum_bits, bits, memory_order_relaxed);
    for (uint32_t i = 0; i < new_inst->num_buckets; i++) {
        if (value <= new_inst->buckets[i].upper_bound) {
            atomic_fetch_add_explicit(&new_inst->buckets[i].count, 1,
                                      memory_order_relaxed);
            break;
        }
    }

    new_inst->next = atomic_load_explicit(&metric->data.histogram.instances,
                                          memory_order_relaxed);
    atomic_store_explicit(&metric->data.histogram.instances, new_inst,
                          memory_order_release);
    pthread_mutex_unlock(&metric->data.histogram.instance_lock);
    return DISTRIC_OK;
}

/* ============================================================================
 * Read-side helpers (tests / HTTP export)
 * ========================================================================= */

uint64_t metrics_counter_get(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_COUNTER) return 0;
    counter_instance_t* inst =
        atomic_load_explicit(&metric->data.counter.instances,
                             memory_order_acquire);
    uint64_t total = 0;
    while (inst) {
        total += atomic_load_explicit(&inst->value, memory_order_relaxed);
        inst = inst->next;
    }
    return total;
}

double metrics_gauge_get(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_GAUGE) return 0.0;
    gauge_instance_t* inst =
        atomic_load_explicit(&metric->data.gauge.instances,
                             memory_order_acquire);
    if (!inst) return 0.0;
    uint64_t bits = atomic_load_explicit(&inst->value_bits, memory_order_relaxed);
    double value;
    memcpy(&value, &bits, sizeof(double));
    return value;
}

uint64_t metrics_histogram_get_count(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) return 0;
    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances,
                             memory_order_acquire);
    uint64_t total = 0;
    while (inst) {
        total += atomic_load_explicit(&inst->count, memory_order_relaxed);
        inst = inst->next;
    }
    return total;
}

double metrics_histogram_get_sum(metric_t* metric) {
    if (!metric || metric->type != METRIC_TYPE_HISTOGRAM) return 0.0;
    histogram_instance_t* inst =
        atomic_load_explicit(&metric->data.histogram.instances,
                             memory_order_acquire);
    if (!inst) return 0.0;
    uint64_t bits = atomic_load_explicit(&inst->sum_bits, memory_order_relaxed);
    double value;
    memcpy(&value, &bits, sizeof(double));
    return value;
}

/* ============================================================================
 * Prometheus text-format export
 * ========================================================================= */

distric_err_t metrics_export_prometheus(metrics_registry_t* registry,
                                        char** out_buffer,
                                        size_t* out_size) {
    if (!registry || !out_buffer || !out_size) return DISTRIC_ERR_INVALID_ARG;

    size_t buffer_size = 65536;
    char* buffer = malloc(buffer_size);
    if (!buffer) return DISTRIC_ERR_NO_MEMORY;

    size_t offset = 0;
    size_t n = atomic_load_explicit(&registry->metric_count,
                                    memory_order_acquire);

    for (size_t mi = 0; mi < n; mi++) {
        metric_t* m = &registry->metrics[mi];
        if (!atomic_load_explicit(&m->initialized, memory_order_acquire))
            continue;

        int written;

        /* Grow buffer if needed (simple doubling). */
#define ENSURE_SPACE(needed)                                          \
        while (offset + (needed) >= buffer_size) {                   \
            buffer_size *= 2;                                         \
            char* nb = realloc(buffer, buffer_size);                 \
            if (!nb) { free(buffer); return DISTRIC_ERR_NO_MEMORY; } \
            buffer = nb;                                              \
        }

        ENSURE_SPACE(512 + MAX_METRIC_NAME_LEN + MAX_METRIC_HELP_LEN);
        written = snprintf(buffer + offset, buffer_size - offset,
                           "# HELP %s %s\n", m->name, m->help);
        if (written > 0) offset += (size_t)written;

        const char* type_str =
            m->type == METRIC_TYPE_COUNTER   ? "counter" :
            m->type == METRIC_TYPE_GAUGE     ? "gauge"   : "histogram";
        ENSURE_SPACE(64 + MAX_METRIC_NAME_LEN);
        written = snprintf(buffer + offset, buffer_size - offset,
                           "# TYPE %s %s\n", m->name, type_str);
        if (written > 0) offset += (size_t)written;

        if (m->type == METRIC_TYPE_COUNTER) {
            counter_instance_t* inst =
                atomic_load_explicit(&m->data.counter.instances,
                                     memory_order_acquire);
            while (inst) {
                ENSURE_SPACE(256 + MAX_METRIC_NAME_LEN);
                if (inst->num_labels == 0) {
                    written = snprintf(buffer + offset, buffer_size - offset,
                                       "%s %lu\n", m->name,
                                       (unsigned long)atomic_load_explicit(
                                           &inst->value, memory_order_relaxed));
                } else {
                    offset += (size_t)snprintf(buffer + offset,
                                               buffer_size - offset,
                                               "%s{", m->name);
                    for (uint32_t li = 0; li < inst->num_labels; li++) {
                        ENSURE_SPACE(128);
                        offset += (size_t)snprintf(
                            buffer + offset, buffer_size - offset,
                            "%s%s=\"%s\"",
                            li > 0 ? "," : "",
                            inst->labels[li].key, inst->labels[li].value);
                    }
                    ENSURE_SPACE(64);
                    written = snprintf(buffer + offset, buffer_size - offset,
                                       "} %lu\n",
                                       (unsigned long)atomic_load_explicit(
                                           &inst->value, memory_order_relaxed));
                }
                if (written > 0) offset += (size_t)written;
                inst = inst->next;
            }

        } else if (m->type == METRIC_TYPE_GAUGE) {
            gauge_instance_t* inst =
                atomic_load_explicit(&m->data.gauge.instances,
                                     memory_order_acquire);
            while (inst) {
                ENSURE_SPACE(256 + MAX_METRIC_NAME_LEN);
                uint64_t bits = atomic_load_explicit(&inst->value_bits,
                                                     memory_order_relaxed);
                double val;
                memcpy(&val, &bits, sizeof(double));
                if (inst->num_labels == 0) {
                    written = snprintf(buffer + offset, buffer_size - offset,
                                       "%s %.6f\n", m->name, val);
                } else {
                    offset += (size_t)snprintf(buffer + offset,
                                               buffer_size - offset,
                                               "%s{", m->name);
                    for (uint32_t li = 0; li < inst->num_labels; li++) {
                        ENSURE_SPACE(128);
                        offset += (size_t)snprintf(
                            buffer + offset, buffer_size - offset,
                            "%s%s=\"%s\"",
                            li > 0 ? "," : "",
                            inst->labels[li].key, inst->labels[li].value);
                    }
                    ENSURE_SPACE(64);
                    written = snprintf(buffer + offset, buffer_size - offset,
                                       "} %.6f\n", val);
                }
                if (written > 0) offset += (size_t)written;
                inst = inst->next;
            }

        } else if (m->type == METRIC_TYPE_HISTOGRAM) {
            histogram_instance_t* inst =
                atomic_load_explicit(&m->data.histogram.instances,
                                     memory_order_acquire);
            while (inst) {
                for (uint32_t bi = 0; bi < inst->num_buckets; bi++) {
                    ENSURE_SPACE(256 + MAX_METRIC_NAME_LEN);
                    double ub = inst->buckets[bi].upper_bound;
                    uint64_t bc = atomic_load_explicit(&inst->buckets[bi].count,
                                                       memory_order_relaxed);
                    if (isinf(ub)) {
                        written = snprintf(buffer + offset,
                                           buffer_size - offset,
                                           "%s_bucket{le=\"+Inf\"} %lu\n",
                                           m->name, (unsigned long)bc);
                    } else {
                        written = snprintf(buffer + offset,
                                           buffer_size - offset,
                                           "%s_bucket{le=\"%g\"} %lu\n",
                                           m->name, ub, (unsigned long)bc);
                    }
                    if (written > 0) offset += (size_t)written;
                }

                ENSURE_SPACE(128 + MAX_METRIC_NAME_LEN);
                uint64_t sum_bits = atomic_load_explicit(&inst->sum_bits,
                                                         memory_order_relaxed);
                double sum;
                memcpy(&sum, &sum_bits, sizeof(double));
                uint64_t cnt = atomic_load_explicit(&inst->count,
                                                    memory_order_relaxed);
                written = snprintf(buffer + offset, buffer_size - offset,
                                   "%s_sum %.6f\n%s_count %lu\n",
                                   m->name, sum, m->name, (unsigned long)cnt);
                if (written > 0) offset += (size_t)written;
                inst = inst->next;
            }
        }
#undef ENSURE_SPACE
    }

    *out_buffer = buffer;
    *out_size   = offset;
    return DISTRIC_OK;
}