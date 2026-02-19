/**
 * @file transport_config.c
 * @brief DistriC Transport — Global Runtime Configuration Implementation
 *
 * All hot-path operations are lock-free via C11 atomics.
 * The only mutex is in transport_config_apply() which is a cold path.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/transport_config.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

/* ============================================================================
 * GLOBAL STATE — atomic, lock-free on hot paths
 * ========================================================================= */

/* Memory accounting */
static _Atomic size_t g_memory_in_use     = 0;
static _Atomic size_t g_memory_limit      = 0; /* 0 = unlimited */
static _Atomic uint64_t g_rejected_memory = 0;

/* Observability sampling (1–100; 100 = emit all) */
static _Atomic uint32_t g_obs_sample_pct  = 100;
static _Atomic uint64_t g_obs_dropped     = 0;

/* Handler warn duration (ms; 0 = disabled) */
static _Atomic uint32_t g_handler_warn_ms = 0;

/* Config update serialisation (write is rare) */
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Metric handles — written once, read under atomic load */
static _Atomic(metric_t *) g_mem_in_use_metric     = NULL;
static _Atomic(metric_t *) g_rejected_mem_metric    = NULL;
static _Atomic(metric_t *) g_obs_dropped_metric     = NULL;

/* ============================================================================
 * transport_config_init
 * ========================================================================= */

void transport_config_init(transport_global_config_t *cfg) {
    if (!cfg) return;
    cfg->max_transport_memory_bytes = 0;
    cfg->observability_sample_pct   = 100;
    cfg->handler_warn_duration_ms   = 0;
}

/* ============================================================================
 * transport_config_apply
 * ========================================================================= */

distric_err_t transport_config_apply(const transport_global_config_t *cfg) {
    if (!cfg) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&g_config_mutex);

    atomic_store_explicit(&g_memory_limit, cfg->max_transport_memory_bytes,
                          memory_order_release);

    uint32_t pct = cfg->observability_sample_pct;
    if (pct == 0)   pct = 100;
    if (pct > 100)  pct = 100;
    atomic_store_explicit(&g_obs_sample_pct, pct, memory_order_release);

    atomic_store_explicit(&g_handler_warn_ms, cfg->handler_warn_duration_ms,
                          memory_order_release);

    pthread_mutex_unlock(&g_config_mutex);
    return DISTRIC_OK;
}

/* ============================================================================
 * transport_config_load_env
 * ========================================================================= */

void transport_config_load_env(transport_global_config_t *cfg) {
    if (!cfg) return;

    const char *v;

    v = getenv("DISTRIC_MAX_TRANSPORT_MEMORY_BYTES");
    if (v && *v) {
        unsigned long long val = strtoull(v, NULL, 10);
        if (val > 0) cfg->max_transport_memory_bytes = (size_t)val;
    }

    v = getenv("DISTRIC_OBS_SAMPLE_PCT");
    if (v && *v) {
        unsigned long val = strtoul(v, NULL, 10);
        if (val >= 1 && val <= 100) cfg->observability_sample_pct = (uint32_t)val;
    }

    v = getenv("DISTRIC_HANDLER_WARN_DURATION_MS");
    if (v && *v) {
        unsigned long val = strtoul(v, NULL, 10);
        if (val > 0) cfg->handler_warn_duration_ms = (uint32_t)val;
    }
}

/* ============================================================================
 * transport_config_get
 * ========================================================================= */

void transport_config_get(transport_global_config_t *out) {
    if (!out) return;
    out->max_transport_memory_bytes = atomic_load_explicit(&g_memory_limit,
                                                           memory_order_acquire);
    out->observability_sample_pct   = atomic_load_explicit(&g_obs_sample_pct,
                                                            memory_order_relaxed);
    out->handler_warn_duration_ms   = atomic_load_explicit(&g_handler_warn_ms,
                                                            memory_order_relaxed);
}

/* ============================================================================
 * transport_config_register_metrics
 * ========================================================================= */

distric_err_t transport_config_register_metrics(metrics_registry_t *registry) {
    if (!registry) return DISTRIC_ERR_INVALID_ARG;

    metric_t *m = NULL;
    distric_err_t rc;

    rc = metrics_register_gauge(registry,
        "transport_memory_bytes_in_use",
        "Total bytes allocated in transport send-queue ring buffers",
        NULL, 0, &m);
    if (rc == DISTRIC_OK && m)
        atomic_store_explicit(&g_mem_in_use_metric, m, memory_order_release);

    m = NULL;
    rc = metrics_register_counter(registry,
        "connections_rejected_memory_total",
        "Connections rejected because the global memory budget was exhausted",
        NULL, 0, &m);
    if (rc == DISTRIC_OK && m)
        atomic_store_explicit(&g_rejected_mem_metric, m, memory_order_release);

    m = NULL;
    rc = metrics_register_counter(registry,
        "observability_dropped_events_total",
        "Transport log/metric events skipped by the sampling rate limiter",
        NULL, 0, &m);
    if (rc == DISTRIC_OK && m)
        atomic_store_explicit(&g_obs_dropped_metric, m, memory_order_release);

    return DISTRIC_OK;
}

/* ============================================================================
 * INTERNAL — hot-path memory accounting (lock-free)
 * ========================================================================= */

bool transport_memory_acquire(size_t bytes) {
    size_t limit = atomic_load_explicit(&g_memory_limit, memory_order_relaxed);
    if (limit == 0) {
        /* Unlimited — just account */
        atomic_fetch_add_explicit(&g_memory_in_use, bytes, memory_order_relaxed);
        return true;
    }

    /*
     * CAS loop: speculatively add, then verify. Under contention each thread
     * retries with the latest value. This is O(1) amortised for typical
     * connection rates.
     */
    size_t current = atomic_load_explicit(&g_memory_in_use, memory_order_relaxed);
    for (;;) {
        if (current + bytes > limit) {
            /* Over budget — reject */
            atomic_fetch_add_explicit(&g_rejected_memory, 1, memory_order_relaxed);
            metric_t *m = atomic_load_explicit(&g_rejected_mem_metric,
                                               memory_order_relaxed);
            if (m) metrics_counter_inc(m);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &g_memory_in_use, &current, current + bytes,
                memory_order_release, memory_order_relaxed))
        {
            /* Successfully reserved */
            metric_t *m = atomic_load_explicit(&g_mem_in_use_metric,
                                               memory_order_relaxed);
            if (m) metrics_gauge_set(m, (int64_t)(current + bytes));
            return true;
        }
        /* CAS failed — current updated by the atomic; retry */
    }
}

void transport_memory_release(size_t bytes) {
    if (bytes == 0) return;
    size_t prev = atomic_fetch_sub_explicit(&g_memory_in_use, bytes,
                                            memory_order_release);
    /* Guard against wrapping (should not happen, but defensive) */
    if (prev < bytes) {
        atomic_store_explicit(&g_memory_in_use, 0, memory_order_release);
    }
    metric_t *m = atomic_load_explicit(&g_mem_in_use_metric, memory_order_relaxed);
    if (m) {
        size_t now = atomic_load_explicit(&g_memory_in_use, memory_order_relaxed);
        metrics_gauge_set(m, (int64_t)now);
    }
}

size_t transport_memory_in_use(void) {
    return atomic_load_explicit(&g_memory_in_use, memory_order_relaxed);
}

/* ============================================================================
 * INTERNAL — observability sampling (lock-free, thread-local LCG)
 * ========================================================================= */

bool transport_should_sample_obs(void) {
    uint32_t pct = atomic_load_explicit(&g_obs_sample_pct, memory_order_relaxed);
    if (pct >= 100) return true;
    if (pct == 0)   return false;

    /*
     * Thread-local LCG (Knuth MMIX) — no locks, no atomics.
     * Initialised to a pointer-derived seed to avoid correlated streams
     * across threads.
     */
    static __thread uint64_t tls_rng = 0;
    if (__builtin_expect(tls_rng == 0, 0)) {
        tls_rng = (uint64_t)(uintptr_t)&tls_rng ^ 0xdeadbeefcafebabeULL;
    }
    tls_rng = tls_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(tls_rng >> 33) % 100u < pct;
}

void transport_obs_record_drop(void) {
    atomic_fetch_add_explicit(&g_obs_dropped, 1, memory_order_relaxed);
    metric_t *m = atomic_load_explicit(&g_obs_dropped_metric, memory_order_relaxed);
    if (m) metrics_counter_inc(m);
}