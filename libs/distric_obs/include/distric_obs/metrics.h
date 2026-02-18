/**
 * @file metrics.h (internal)
 * @brief DistriC Observability — Metrics Internal Implementation Details
 *
 * NOT part of the public API.  Only metrics.c may include this header.
 *
 * =============================================================================
 * PRODUCTION HARDENING APPLIED
 * =============================================================================
 *
 * #1 Memory-Ordering Annotations
 *   - counter_instance.value: fetch_add(relaxed) — pure counter.
 *   - gauge_instance.value_bits: CAS(relaxed, relaxed) — float exchange loop.
 *   - histogram bucket.count: fetch_add(relaxed) — pure counter.
 *   - histogram.sum_bits: CAS(relaxed, relaxed) — float accumulate.
 *   - registry.state: store(release) on freeze; load(acquire) on hot-path check.
 *   - registry.metric_count: store(release) after registration; load(acquire)
 *     during export.
 *
 * #3 Cache Line Alignment
 *   - counter_instance.value on its own alignas(64) cache line.
 *   - gauge_instance.value_bits on its own alignas(64) cache line.
 *   - histogram_instance.count/sum_bits on their own alignas(64) region.
 *
 * #4 Label Resolution Performance
 *   PREVIOUS: linked-list O(n) scan per hot-path label lookup.
 *   NEW: precomputed flat Cartesian index array.
 *
 *   At registration time:
 *     - compute_strides() fills metric_t.strides[].
 *     - An instance_array of size `cardinality` is heap-allocated.
 *     - A labels_table mapping flat index → label set is built once.
 *
 *   At hot-path time:
 *     - compute_flat_index() maps label values → flat index in O(D).
 *     - Access instance_array[flat_index] → atomic operation.
 *     - No mutex; no scan; no allocation.
 */

#ifndef DISTRIC_METRICS_INTERNAL_H
#define DISTRIC_METRICS_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdalign.h>

/* ============================================================================
 * Compile-time internal defaults
 * ========================================================================= */

#define MAX_METRICS            DISTRIC_MAX_METRICS
#define MAX_METRIC_LABELS      DISTRIC_MAX_METRIC_LABELS
#define MAX_METRIC_NAME_LEN    DISTRIC_MAX_METRIC_NAME_LEN
#define MAX_METRIC_HELP_LEN    DISTRIC_MAX_METRIC_HELP_LEN
#define MAX_LABEL_KEY_LEN      DISTRIC_MAX_LABEL_KEY_LEN
#define MAX_LABEL_VALUE_LEN    DISTRIC_MAX_LABEL_VALUE_LEN
#define MAX_METRIC_CARDINALITY DISTRIC_MAX_METRIC_CARDINALITY

#define HISTOGRAM_BUCKET_COUNT 10u

/* ============================================================================
 * Registry state machine
 * ========================================================================= */

typedef enum {
    REGISTRY_STATE_MUTABLE   = 0,
    REGISTRY_STATE_FROZEN    = 1,
    REGISTRY_STATE_DESTROYED = 2,
} registry_state_t;

/* ============================================================================
 * Internal instance types (Item #3: hot atomics on own cache lines)
 *
 * Each instance type places its hot atomic field(s) on a separate alignas(64)
 * region to prevent false sharing between threads updating different instances.
 *
 * Label fields are read-only after init; they reside before the hot field and
 * are evicted separately from the counter/gauge cache line.
 * ========================================================================= */

typedef struct {
    /* Hot: updated on every counter inc */
    alignas(64) _Atomic uint64_t value;  /* relaxed fetch_add */
} counter_instance_t;

typedef struct {
    /* Hot: updated on every gauge set */
    alignas(64) _Atomic uint64_t value_bits;  /* CAS relaxed/relaxed; IEEE 754 double */
} gauge_instance_t;

typedef struct {
    double           upper_bound;
    _Atomic uint64_t count;   /* relaxed fetch_add */
} histogram_bucket_t;

typedef struct {
    histogram_bucket_t* buckets;
    uint32_t            num_buckets;
    /* Hot: updated on every observation */
    alignas(64) _Atomic uint64_t count;     /* relaxed fetch_add */
    _Atomic uint64_t             sum_bits;  /* CAS relaxed/relaxed; IEEE 754 double */
} histogram_instance_t;

/* ============================================================================
 * Instance array union (Item #4: flat Cartesian array replaces linked list)
 * ========================================================================= */

typedef union {
    counter_instance_t*   counter;
    gauge_instance_t*     gauge;
    histogram_instance_t* histogram;
} instance_array_t;

/* ============================================================================
 * metric_s_fields_t: common prefix for compute_flat_index helper.
 *
 * compute_flat_index() casts metric_t* to metric_s_fields_t* to access
 * label_defs, strides, and cardinality without a full forward decl.
 * The cast is safe because metric_t begins with exactly these fields
 * in the same order.
 * ========================================================================= */

typedef struct {
    char                      name[MAX_METRIC_NAME_LEN];
    char                      help[MAX_METRIC_HELP_LEN];
    metric_type_t             type;
    metric_label_definition_t label_defs[MAX_METRIC_LABELS];
    uint32_t                  num_label_defs;
    _Atomic bool              initialized;
    size_t                    strides[MAX_METRIC_LABELS];  /* Cartesian strides */
    size_t                    cardinality;                 /* instance count    */
} metric_s_fields_t;

/* ============================================================================
 * Full metric_t
 * ========================================================================= */

struct metric_s {
    /* === Static metadata (read-only after registration) === */

    char              name[MAX_METRIC_NAME_LEN];
    char              help[MAX_METRIC_HELP_LEN];
    metric_type_t     type;

    /* Label definitions: key + allowed_values allowlist */
    metric_label_definition_t label_defs[MAX_METRIC_LABELS];
    uint32_t          num_label_defs;

    _Atomic bool      initialized;

    /*
     * strides[i] = product of num_allowed_values[j] for j > i.
     * Used by compute_flat_index() to map label values → flat array index.
     * Computed once at registration; never modified after.
     */
    size_t            strides[MAX_METRIC_LABELS];

    /*
     * cardinality = product of all num_allowed_values.
     * Size of instance_array; equals 1 for unlabelled metrics.
     */
    size_t            cardinality;

    /*
     * labels_table: flat array of (cardinality × num_label_defs) metric_label_t.
     * labels_table[fi * num_label_defs .. fi * num_label_defs + num_label_defs - 1]
     * gives the label set for flat index fi.
     * Allocated once at registration; used during Prometheus export.
     * NULL for unlabelled metrics.
     */
    void*             labels_table;

    /* === Instance arrays (Item #4: flat Cartesian array) === */

    instance_array_t  instance_array;

    /* Histogram-specific: bucket template */
    uint32_t          num_buckets;

    /*
     * instance_lock: guards lazy creation paths.
     * With the array-based design, all instances are pre-allocated at
     * registration time.  This lock is kept for forward-compatibility
     * and debug assertions only.
     */
    pthread_mutex_t   instance_lock;
};

/* ============================================================================
 * Registry
 * ========================================================================= */

struct metrics_registry_s {
    metric_t         metrics[MAX_METRICS];
    size_t           effective_max;
    size_t           effective_cardinality_cap;

    /*
     * metric_count: store(release) after registration (under mutex);
     *               load(acquire) during export.
     */
    _Atomic size_t   metric_count;

    /*
     * state: store(release) on freeze;
     *        load(acquire) on registration to enforce frozen invariant.
     */
    _Atomic uint32_t state;

    _Atomic uint32_t refcount;
    pthread_mutex_t  register_mutex;
};

/* ============================================================================
 * Lifecycle assertion helpers (Item #1: detect use-after-free / double-free)
 * ========================================================================= */

#ifdef NDEBUG
#  define METRICS_ASSERT_LIFECYCLE(cond) ((void)0)
#else
#  include <stdio.h>
#  include <stdlib.h>
#  define METRICS_ASSERT_LIFECYCLE(cond)                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "[distric_obs] LIFECYCLE VIOLATION: %s "          \
                    "(%s:%d)\n", #cond, __FILE__, __LINE__);                   \
            abort();                                                            \
        }                                                                      \
    } while (0)
#endif

#endif /* DISTRIC_METRICS_INTERNAL_H */