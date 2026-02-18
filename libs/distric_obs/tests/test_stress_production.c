/*
 * test_stress_production.c
 * DistriC Observability Library — Production Blocker Stress Tests
 *
 * Covers all three production blockers:
 *
 *   P1. Strict metric label cardinality enforcement
 *       P1a: Reject registration with NULL allowlist dimension
 *       P1b: Reject registration exceeding MAX_METRIC_CARDINALITY
 *       P1c: Reject updates with values outside the allowlist
 *       P1d: Concurrent valid/invalid updates — all invalid ones rejected
 *
 *   P2. Tracing adaptive sampling under sustained backpressure
 *       P2a: Multi-threaded producers with stalled exporter
 *       P2b: trace_get_stats() reflects degradation
 *       P2c: trace_register_metrics() wires to Prometheus
 *
 *   P3. Non-blocking guarantees under sustained pressure
 *       P3a: Metric write storm — per-op latency bounded
 *       P3b: Tracing with saturated buffer — finish_span never blocks
 *       P3c: Logger sustained load — log_write never blocks
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdatomic.h>
#include <math.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * P1a. Reject registration with NULL allowlist dimension
 * ========================================================================= */

static void test_p1a_reject_null_allowlist(void) {
    printf("Test [P1a]: Reject registration with NULL allowlist...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    /* NULL allowed_values = unbounded = must be rejected */
    metric_label_definition_t def = {
        .key              = "method",
        .allowed_values   = NULL,
        .num_allowed_values = 0,
    };
    metric_t* m = NULL;
    distric_err_t err = metrics_register_counter(reg, "bad_null", "test",
                                                 &def, 1, &m);
    assert(err == DISTRIC_ERR_HIGH_CARDINALITY && "NULL allowlist must be rejected");
    assert(m == NULL);
    printf("  NULL allowlist correctly rejected with DISTRIC_ERR_HIGH_CARDINALITY\n");

    /* 0 allowed values = unbounded */
    const char* empty[1] = { NULL }; 
    metric_label_definition_t def2 = {
        .key = "status", .allowed_values = empty, .num_allowed_values = 0,
    };
    err = metrics_register_counter(reg, "bad_empty", "test", &def2, 1, &m);
    assert(err == DISTRIC_ERR_HIGH_CARDINALITY && "Empty allowlist must be rejected");
    printf("  Empty allowlist correctly rejected\n");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P1b. Reject registration exceeding MAX_METRIC_CARDINALITY
 * ========================================================================= */

static void test_p1b_reject_high_cardinality(void) {
    printf("Test [P1b]: Reject registration exceeding MAX_METRIC_CARDINALITY...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    /* 4^DISTRIC_MAX_METRIC_LABELS = 4^8 = 65536 > 10000 = DISTRIC_MAX_METRIC_CARDINALITY */
    const char* vals[] = {"a","b","c","d"};
    metric_label_definition_t defs[DISTRIC_MAX_METRIC_LABELS];
    for (int i = 0; i < DISTRIC_MAX_METRIC_LABELS; i++) {
        snprintf(defs[i].key, DISTRIC_MAX_LABEL_KEY_LEN, "dim%d", i);
        defs[i].allowed_values    = vals;
        defs[i].num_allowed_values = 4;
    }
    metric_t* m = NULL;
    distric_err_t err = metrics_register_counter(reg, "over_cap", "test",
                                                 defs, DISTRIC_MAX_METRIC_LABELS, &m);
    assert(err == DISTRIC_ERR_HIGH_CARDINALITY &&
           "4^DISTRIC_MAX_METRIC_LABELS > DISTRIC_MAX_METRIC_CARDINALITY must be rejected");
    assert(m == NULL);
    printf("  4^%d=%llu combinations correctly rejected\n",
           DISTRIC_MAX_METRIC_LABELS,
           (unsigned long long)1 << (2 * DISTRIC_MAX_METRIC_LABELS)); /* 4^n = 2^(2n) */

    /* 4^3 = 64 <= 256 — must succeed */
    metric_t* ok = NULL;
    err = metrics_register_counter(reg, "under_cap", "test", defs, 3, &ok);
    assert(err == DISTRIC_OK &&
           "4^3=64 combinations must be accepted (64 < DISTRIC_MAX_METRIC_CARDINALITY)");
    assert(ok != NULL);
    printf("  4^3=64 combinations correctly accepted\n");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P1c. Reject updates with labels outside allowlist
 * ========================================================================= */

static void test_p1c_reject_invalid_label_updates(void) {
    printf("Test [P1c]: Reject updates with labels outside allowlist...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    const char* methods[]  = {"GET", "POST", "PUT", "DELETE"};
    const char* statuses[] = {"200", "400", "404", "500"};
    metric_label_definition_t defs[2] = {
        { .key="method", .allowed_values=methods,  .num_allowed_values=4 },
        { .key="status", .allowed_values=statuses, .num_allowed_values=4 },
    };
    metric_t* c = NULL;
    assert(metrics_register_counter(reg, "api_req", "api", defs, 2, &c) == DISTRIC_OK);

    /* Valid update */
    metric_label_t valid[] = {{"method","GET"},{"status","200"}};
    assert(metrics_counter_inc_with_labels(c, valid, 2, 1) == DISTRIC_OK);

    /* Value not in allowlist */
    metric_label_t bad_val[] = {{"method","OPTIONS"},{"status","200"}};
    distric_err_t err = metrics_counter_inc_with_labels(c, bad_val, 2, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL && "'OPTIONS' not in method allowlist");
    printf("  Invalid value 'OPTIONS' correctly rejected\n");

    /* Key not defined */
    metric_label_t bad_key[] = {{"region","us-east"},{"status","200"}};
    err = metrics_counter_inc_with_labels(c, bad_key, 2, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL && "key 'region' not defined");
    printf("  Unknown key 'region' correctly rejected\n");

    /* Too many labels */
    metric_label_t too_many[] = {{"method","GET"},{"status","200"},{"extra","v"}};
    err = metrics_counter_inc_with_labels(c, too_many, 3, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL && "excess labels must be rejected");
    printf("  Excess label count correctly rejected\n");

    /* Counter must reflect only the one valid update */
    assert(metrics_counter_get(c) == 1 && "only valid update must be counted");
    printf("  Counter value == 1 (only valid update counted)\n");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P1d. Concurrent valid/invalid updates
 * ========================================================================= */

#define P1D_THREADS 16
#define P1D_UPDATES 500

typedef struct { metric_t* counter; int valid_count; int invalid_count; } p1d_arg_t;

static void* p1d_worker(void* arg) {
    p1d_arg_t* a = (p1d_arg_t*)arg;
    metric_label_t good[] = {{"method","GET"},   {"status","200"}};
    metric_label_t bad[]  = {{"method","TRACE"}, {"status","200"}};
    for (int i = 0; i < P1D_UPDATES; i++) {
        if (metrics_counter_inc_with_labels(a->counter, good, 2, 1) == DISTRIC_OK)
            a->valid_count++;
        if (metrics_counter_inc_with_labels(a->counter, bad, 2, 1) == DISTRIC_ERR_INVALID_LABEL)
            a->invalid_count++;
    }
    return NULL;
}

static void test_p1d_concurrent_enforcement(void) {
    printf("Test [P1d]: Concurrent valid/invalid updates (%d threads x %d ops)...\n",
           P1D_THREADS, P1D_UPDATES);

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);
    const char* methods[]  = {"GET","POST","PUT","DELETE"};
    const char* statuses[] = {"200","400","404","500"};
    metric_label_definition_t defs[2] = {
        { .key="method", .allowed_values=methods,  .num_allowed_values=4 },
        { .key="status", .allowed_values=statuses, .num_allowed_values=4 },
    };
    metric_t* c = NULL;
    assert(metrics_register_counter(reg, "concurrent_c", "test", defs, 2, &c) == DISTRIC_OK);

    pthread_t threads[P1D_THREADS];
    p1d_arg_t args[P1D_THREADS];
    for (int i = 0; i < P1D_THREADS; i++) {
        args[i] = (p1d_arg_t){ .counter = c };
        pthread_create(&threads[i], NULL, p1d_worker, &args[i]);
    }
    for (int i = 0; i < P1D_THREADS; i++) pthread_join(threads[i], NULL);

    int total_valid = 0, total_invalid = 0;
    for (int i = 0; i < P1D_THREADS; i++) {
        total_valid   += args[i].valid_count;
        total_invalid += args[i].invalid_count;
    }
    int expected = P1D_THREADS * P1D_UPDATES;
    assert(total_valid   == expected && "All valid updates must be counted");
    assert(total_invalid == expected && "All invalid updates must be rejected");
    assert(metrics_counter_get(c) == (uint64_t)expected &&
           "Counter must equal valid update count");
    printf("  valid=%d invalid_rejected=%d counter=%lu  OK\n",
           total_valid, total_invalid, (unsigned long)metrics_counter_get(c));

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P2a. Sustained backpressure + adaptive sampling
 * ========================================================================= */

#define P2A_PRODUCERS   8
#define P2A_SPANS_PER_THREAD 4000
#define P2A_WALL_LIMIT_NS 30000000000ULL  /* 30 s */

static void noop_export(trace_span_t* s, size_t n, void* ud) {
    (void)s; (void)n; (void)ud;
    /* Extremely slow exporter — stall for 50 ms per batch. */
    struct timespec ts = { 0, 50000000L };
    nanosleep(&ts, NULL);
}

typedef struct { tracer_t* tracer; } p2a_arg_t;

static void* p2a_producer(void* arg) {
    tracer_t* t = ((p2a_arg_t*)arg)->tracer;
    for (int i = 0; i < P2A_SPANS_PER_THREAD; i++) {
        trace_span_t* span = NULL;
        trace_start_span(t, "work", &span);
        if (span && span->sampled)
            trace_finish_span(t, span);
    }
    return NULL;
}

static void test_p2a_sustained_overload(void) {
    printf("Test [P2a]: Sustained backpressure (%d producers, stalled exporter)...\n",
           P2A_PRODUCERS);

    trace_sampling_config_t cfg = { .always_sample=1, .always_drop=0,
                                    .backpressure_sample=1, .backpressure_drop=9 };
    tracer_t* t;
    assert(trace_init_with_sampling(&t, &cfg, noop_export, NULL) == DISTRIC_OK);

    pthread_t threads[P2A_PRODUCERS];
    p2a_arg_t args[P2A_PRODUCERS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < P2A_PRODUCERS; i++) {
        args[i] = (p2a_arg_t){ .tracer = t };
        pthread_create(&threads[i], NULL, p2a_producer, &args[i]);
    }
    for (int i = 0; i < P2A_PRODUCERS; i++) pthread_join(threads[i], NULL);
    uint64_t flood_ns = now_ns() - t0;

    assert(flood_ns < P2A_WALL_LIMIT_NS &&
           "Producer threads exceeded wall-clock bound — potential blocking");

    tracer_stats_t stats;
    trace_get_stats(t, &stats);
    printf("  created=%lu sampled_in=%lu sampled_out=%lu dropped=%lu "
           "bp=%s rate=%u%%\n",
           (unsigned long)stats.spans_created,
           (unsigned long)stats.spans_sampled_in,
           (unsigned long)stats.spans_sampled_out,
           (unsigned long)stats.spans_dropped_backpressure,
           stats.in_backpressure ? "YES" : "NO",
           stats.effective_sample_rate_pct);

    assert(stats.spans_created == (uint64_t)(P2A_PRODUCERS * P2A_SPANS_PER_THREAD) &&
           "spans_created must equal all producer attempts");

    bool degradation = (stats.spans_sampled_out > 0 ||
                        stats.spans_dropped_backpressure > 0);
    assert(degradation && "Sustained overload must produce observable degradation");
    printf("  Degradation: sampled_out=%lu drops=%lu  OK\n",
           (unsigned long)stats.spans_sampled_out,
           (unsigned long)stats.spans_dropped_backpressure);

    trace_destroy(t);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P2b. trace_get_stats() reflects backpressure transitions
 * ========================================================================= */

static void stall_export(trace_span_t* s, size_t n, void* ud) {
    (void)s; (void)n; (void)ud;
    struct timespec ts = { 0, 50000000L };
    nanosleep(&ts, NULL);
}

static void test_p2b_stats_api(void) {
    printf("Test [P2b]: trace_get_stats() reflects degradation state...\n");

    trace_sampling_config_t cfg = { 1, 0, 1, 9 };
    tracer_t* t;
    assert(trace_init_with_sampling(&t, &cfg, stall_export, NULL) == DISTRIC_OK);

    /* Flood to fill the buffer */
    for (int i = 0; i < 5000; i++) {
        trace_span_t* span = NULL;
        trace_start_span(t, "flood", &span);
        if (span && span->sampled)
            trace_finish_span(t, span);
    }

    tracer_stats_t s;
    trace_get_stats(t, &s);
    printf("  queue=%lu/%lu bp=%s rate=%u%%\n",
           (unsigned long)s.queue_depth, (unsigned long)s.queue_capacity,
           s.in_backpressure ? "YES" : "NO", s.effective_sample_rate_pct);
    printf("  drops=%lu sampled_out=%lu\n",
           (unsigned long)s.spans_dropped_backpressure,
           (unsigned long)s.spans_sampled_out);

    /* Under heavy flood, degradation must have occurred. */
    assert((s.spans_dropped_backpressure > 0 || s.spans_sampled_out > 0) &&
           "Stats must show degradation after buffer flood");

    trace_destroy(t);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P2c. trace_register_metrics() wires to Prometheus
 * ========================================================================= */

static void noop_cb(trace_span_t* s, size_t n, void* ud) { (void)s;(void)n;(void)ud; }

static void test_p2c_register_metrics(void) {
    printf("Test [P2c]: trace_register_metrics() Prometheus wiring...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    tracer_t* t;
    assert(trace_init(&t, noop_cb, NULL) == DISTRIC_OK);

    distric_err_t err = trace_register_metrics(t, reg);
    assert(err == DISTRIC_OK && "trace_register_metrics must succeed");

    /* Emit spans to trigger gauge updates */
    for (int i = 0; i < 20; i++) {
        trace_span_t* span = NULL;
        trace_start_span(t, "test", &span);
        if (span && span->sampled)
            trace_finish_span(t, span);
    }

    char* prom = NULL;
    size_t sz = 0;
    assert(metrics_export_prometheus(reg, &prom, &sz) == DISTRIC_OK);
    assert(prom != NULL);
    /* Queue-depth gauge must appear in output */
    assert(strstr(prom, "distric_internal_tracer") != NULL &&
           "Tracing metrics must appear in Prometheus output");
    printf("  Prometheus output contains tracing metrics\n");
    free(prom);

    trace_destroy(t);
    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P3a. Metric write storm — per-operation latency bounded
 * ========================================================================= */

#define P3A_THREADS   16
#define P3A_OPS       2000
#define P3A_MAX_NS    200000LL  /* 200 µs */

typedef struct { metric_t* counter; uint64_t max_ns; } p3a_arg_t;

static void* p3a_worker(void* arg) {
    p3a_arg_t* a = (p3a_arg_t*)arg;
    for (int i = 0; i < P3A_OPS; i++) {
        uint64_t t0 = now_ns();
        metrics_counter_inc(a->counter);
        uint64_t dur = now_ns() - t0;
        if (dur > a->max_ns) a->max_ns = dur;
    }
    return NULL;
}

static void test_p3a_metric_storm_nonblocking(void) {
    printf("Test [P3a]: Metric write storm non-blocking (%d threads x %d ops)...\n",
           P3A_THREADS, P3A_OPS);

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);
    metric_t* c = NULL;
    assert(metrics_register_counter(reg, "storm_c", "storm", NULL, 0, &c) == DISTRIC_OK);
    /* Warmup to avoid first-use mutex spike in timing. */
    metrics_counter_inc(c);

    pthread_t  threads[P3A_THREADS];
    p3a_arg_t  args[P3A_THREADS];
    for (int i = 0; i < P3A_THREADS; i++) {
        args[i] = (p3a_arg_t){ .counter = c, .max_ns = 0 };
        pthread_create(&threads[i], NULL, p3a_worker, &args[i]);
    }
    for (int i = 0; i < P3A_THREADS; i++) pthread_join(threads[i], NULL);

    uint64_t global_max = 0;
    for (int i = 0; i < P3A_THREADS; i++)
        if (args[i].max_ns > global_max) global_max = args[i].max_ns;

    uint64_t expected = (uint64_t)P3A_THREADS * P3A_OPS + 1;
    assert(metrics_counter_get(c) == expected &&
           "Counter must equal total increments");
    printf("  counter=%lu max_latency=%lu ns  OK\n",
           (unsigned long)metrics_counter_get(c), (unsigned long)global_max);
    assert(global_max < (uint64_t)P3A_MAX_NS &&
           "metrics_counter_inc exceeded 200µs — potential blocking");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P3b. Tracing on saturated buffer — finish_span never blocks
 * ========================================================================= */

#define P3B_THREADS   8
#define P3B_SPANS     500
#define P3B_MAX_NS    100000LL  /* 100 µs */

static _Atomic int p3b_stop = 0;

static void p3b_slow_export(trace_span_t* s, size_t n, void* ud) {
    (void)s; (void)n; (void)ud;
    for (int i = 0; i < 20 && !atomic_load(&p3b_stop); i++) {
        struct timespec ts = { 0, 5000000L };
        nanosleep(&ts, NULL);
    }
}

typedef struct { tracer_t* tracer; uint64_t max_finish_ns; } p3b_arg_t;

static void* p3b_producer(void* arg) {
    p3b_arg_t* a = (p3b_arg_t*)arg;
    for (int i = 0; i < P3B_SPANS; i++) {
        trace_span_t* span = NULL;
        trace_start_span(a->tracer, "sat", &span);
        if (span && span->sampled) {
            uint64_t t0  = now_ns();
            trace_finish_span(a->tracer, span);
            uint64_t dur = now_ns() - t0;
            if (dur > a->max_finish_ns) a->max_finish_ns = dur;
        }
    }
    return NULL;
}

static void test_p3b_trace_saturation_nonblocking(void) {
    printf("Test [P3b]: Tracing non-blocking on saturated buffer "
           "(%d threads x %d spans)...\n", P3B_THREADS, P3B_SPANS);

    atomic_store(&p3b_stop, 0);
    trace_sampling_config_t cfg = { 1, 0, 1, 0 };
    tracer_t* t;
    assert(trace_init_with_sampling(&t, &cfg, p3b_slow_export, NULL) == DISTRIC_OK);

    /* Pre-fill */
    for (int i = 0; i < 2000; i++) {
        trace_span_t* s = NULL;
        if (trace_start_span(t, "fill", &s) == DISTRIC_OK && s && s->sampled)
            trace_finish_span(t, s);
    }
    usleep(10000);

    pthread_t threads[P3B_THREADS];
    p3b_arg_t args[P3B_THREADS];
    for (int i = 0; i < P3B_THREADS; i++) {
        args[i] = (p3b_arg_t){ .tracer = t, .max_finish_ns = 0 };
        pthread_create(&threads[i], NULL, p3b_producer, &args[i]);
    }
    for (int i = 0; i < P3B_THREADS; i++) pthread_join(threads[i], NULL);

    uint64_t global_max = 0;
    for (int i = 0; i < P3B_THREADS; i++)
        if (args[i].max_finish_ns > global_max) global_max = args[i].max_finish_ns;

    printf("  max trace_finish_span latency: %lu ns\n", (unsigned long)global_max);
    assert(global_max < (uint64_t)P3B_MAX_NS &&
           "trace_finish_span on full buffer exceeded 100µs — potential blocking");

    atomic_store(&p3b_stop, 1);
    trace_destroy(t);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P3c. Logger sustained stress — log_write never blocks
 * ========================================================================= */

#define P3C_THREADS  16
#define P3C_LOGS     500
#define P3C_MAX_NS   500000LL  /* 500 µs — still definitively non-blocking (never ms-range) */

typedef struct { logger_t* logger; uint64_t max_ns; int dropped; } p3c_arg_t;

static void* p3c_log_worker(void* arg) {
    p3c_arg_t* a = (p3c_arg_t*)arg;
    for (int i = 0; i < P3C_LOGS; i++) {
        uint64_t t0  = now_ns();
        distric_err_t err = LOG_INFO(a->logger, "stress", "sustained pressure",
                                     "k1", "v1", "k2", "v2", NULL);
        uint64_t dur = now_ns() - t0;
        if (dur > a->max_ns) a->max_ns = dur;
        if (err == DISTRIC_ERR_BUFFER_OVERFLOW) a->dropped++;
    }
    return NULL;
}

static void test_p3c_logger_sustained_nonblocking(void) {
    printf("Test [P3c]: Logger sustained non-blocking (%d threads x %d logs)...\n",
           P3C_THREADS, P3C_LOGS);

    int devnull = open("/dev/null", O_WRONLY);
    assert(devnull >= 0);

    logger_t* lg;
    assert(log_init(&lg, devnull, LOG_MODE_ASYNC) == DISTRIC_OK);

    pthread_t threads[P3C_THREADS];
    p3c_arg_t args[P3C_THREADS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < P3C_THREADS; i++) {
        args[i] = (p3c_arg_t){ .logger = lg };
        pthread_create(&threads[i], NULL, p3c_log_worker, &args[i]);
    }
    for (int i = 0; i < P3C_THREADS; i++) pthread_join(threads[i], NULL);
    uint64_t wall = now_ns() - t0;

    uint64_t global_max = 0;
    int total_dropped = 0;
    for (int i = 0; i < P3C_THREADS; i++) {
        if (args[i].max_ns > global_max) global_max = args[i].max_ns;
        total_dropped += args[i].dropped;
    }

    printf("  max log_write latency: %lu ns  dropped: %d/%d  wall=%.1f ms\n",
           (unsigned long)global_max, total_dropped,
           P3C_THREADS * P3C_LOGS, (double)wall / 1e6);

    assert(global_max < (uint64_t)P3C_MAX_NS &&
           "log_write exceeded 500µs — potential blocking under load");

    log_destroy(lg);
    close(devnull);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Observability — Production Blocker Stress Tests ===\n\n");

    test_p1a_reject_null_allowlist();
    test_p1b_reject_high_cardinality();
    test_p1c_reject_invalid_label_updates();
    test_p1d_concurrent_enforcement();

    test_p2a_sustained_overload();
    test_p2b_stats_api();
    test_p2c_register_metrics();

    test_p3a_metric_storm_nonblocking();
    test_p3b_trace_saturation_nonblocking();
    test_p3c_logger_sustained_nonblocking();

    printf("=== ALL PRODUCTION BLOCKER TESTS PASSED ===\n");
    return 0;
}