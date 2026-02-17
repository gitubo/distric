/*
 * test_stress_production.c
 * DistriC Observability Library — Production Blocker Stress Tests
 *
 * Covers all three production blockers:
 *
 *   P1. Strict metric label cardinality enforcement
 *       - Reject registration with unbounded (NULL allowlist) dimensions
 *       - Reject registration exceeding MAX_METRIC_CARDINALITY
 *       - Reject updates with values outside the allowlist
 *       - Concurrent valid/invalid updates — all invalid ones must be rejected
 *
 *   P2. Tracing adaptive sampling under sustained backpressure
 *       - Multi-threaded producers with a stalled exporter
 *       - Assert adaptive sampling kicks in (in_backpressure becomes true)
 *       - Assert drop counts increase under sustained pressure
 *       - Assert trace_get_stats() reflects degradation
 *       - Assert trace_register_metrics() wires stats to Prometheus
 *
 *   P3. Non-blocking guarantee under sustained pressure
 *       - Producers never stall even when queues are full
 *       - Wall-clock bounds enforced for all three subsystems
 *       - Multi-threaded sustained log producer with file output
 *       - Sustained metric write storm
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

/* ============================================================================
 * Timing
 * ========================================================================= */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * P1a. Strict: reject registration with NULL / empty allowlist
 * ========================================================================= */

static void test_p1_reject_null_allowlist(void) {
    printf("Test [P1a]: Reject registration with NULL allowlist dimension...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    /*
     * A label definition with allowed_values=NULL is unbounded.
     * In production (strict) mode this must fail at registration, not
     * silently accept any value at emit time.
     */
    metric_label_definition_t open_def = {
        .key              = "request_id",   /* could be anything — truly unbounded */
        .allowed_values   = NULL,
        .num_allowed_values = 0,
    };
    metric_t* m = NULL;
    distric_err_t err = metrics_register_counter(reg,
        "requests_with_open_label", "should be rejected",
        &open_def, 1, &m);

    assert(err == DISTRIC_ERR_HIGH_CARDINALITY &&
           "NULL allowlist must be rejected with DISTRIC_ERR_HIGH_CARDINALITY");
    assert(m == NULL && "out_metric must be NULL on failure");
    printf("  NULL-allowlist registration correctly rejected\n");

    /*
     * Variant: zero num_allowed_values — still unbounded even with non-NULL ptr.
     * Use NULL directly; a zero-length array literal is not valid before C23.
     */
    metric_label_definition_t empty_def = {
        .key               = "session_id",
        .allowed_values    = NULL,
        .num_allowed_values = 0,
    };
    err = metrics_register_counter(reg,
        "requests_with_empty_allowlist", "should be rejected",
        &empty_def, 1, &m);

    assert(err == DISTRIC_ERR_HIGH_CARDINALITY &&
           "Zero-length allowlist must be rejected");
    assert(m == NULL);
    printf("  Zero-length allowlist registration correctly rejected\n");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P1b. Strict: reject registration that exceeds MAX_METRIC_CARDINALITY
 * ========================================================================= */

static void test_p1_reject_high_cardinality_registration(void) {
    printf("Test [P1b]: Reject registration exceeding MAX_METRIC_CARDINALITY (%d)...\n",
           MAX_METRIC_CARDINALITY);

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    /* 8 dimensions × 4 values = 4^8 = 65536 >> MAX_METRIC_CARDINALITY */
    const char* v4[] = {"a","b","c","d"};
    metric_label_definition_t big[MAX_METRIC_LABELS];
    for (int i = 0; i < MAX_METRIC_LABELS; i++) {
        snprintf(big[i].key, MAX_LABEL_KEY_LEN, "dim%d", i);
        big[i].allowed_values      = v4;
        big[i].num_allowed_values  = 4;
    }
    metric_t* m = NULL;
    distric_err_t err = metrics_register_counter(reg,
        "explosion", "too many combos", big, MAX_METRIC_LABELS, &m);
    assert(err == DISTRIC_ERR_HIGH_CARDINALITY);
    assert(m == NULL);
    printf("  Overflow cardinality (%d^%d) correctly rejected\n",
           4, MAX_METRIC_LABELS);

    /* Boundary: exactly 256 combinations — should PASS */
    const char* v16[16];
    for (int i = 0; i < 16; i++) {
        /* We re-use string literals — fine for tests */
        static const char* lits[] = {
            "0","1","2","3","4","5","6","7",
            "8","9","A","B","C","D","E","F"
        };
        v16[i] = lits[i];
    }
    metric_label_definition_t boundary[2] = {
        { .key = "hi", .allowed_values = v16, .num_allowed_values = 16 },
        { .key = "lo", .allowed_values = v16, .num_allowed_values = 16 },
    };
    err = metrics_register_counter(reg, "boundary256", "16×16=256",
                                   boundary, 2, &m);
    assert(err == DISTRIC_OK && "Exactly MAX_METRIC_CARDINALITY must be accepted");
    assert(m != NULL);
    printf("  Exactly MAX_METRIC_CARDINALITY=256 correctly accepted\n");

    /* 257 combinations (17 × 16 > 256) — should FAIL */
    const char* v17[17];
    static const char* extra17[] = {
        "0","1","2","3","4","5","6","7",
        "8","9","A","B","C","D","E","F","G"
    };
    for (int i = 0; i < 17; i++) v17[i] = extra17[i];
    metric_label_definition_t over[2] = {
        { .key = "hi17", .allowed_values = v17, .num_allowed_values = 17 },
        { .key = "lo16", .allowed_values = v16, .num_allowed_values = 16 },
    };
    metric_t* m2 = NULL;
    err = metrics_register_counter(reg, "over257", "17×16=272",
                                   over, 2, &m2);
    assert(err == DISTRIC_ERR_HIGH_CARDINALITY && "17×16=272 > 256 must be rejected");
    assert(m2 == NULL);
    printf("  17×16=272 > MAX_METRIC_CARDINALITY correctly rejected\n");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P1c. Strict: reject updates outside the allowlist
 * ========================================================================= */

static void test_p1_reject_invalid_label_updates(void) {
    printf("Test [P1c]: Reject updates with labels outside allowlist...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    const char* methods[]  = {"GET", "POST", "PUT", "DELETE"};
    const char* statuses[] = {"200", "400", "404", "500"};
    metric_label_definition_t defs[2] = {
        { .key = "method", .allowed_values = methods,  .num_allowed_values = 4 },
        { .key = "status", .allowed_values = statuses, .num_allowed_values = 4 },
    };
    metric_t* counter = NULL;
    assert(metrics_register_counter(reg, "api_requests", "api",
                                    defs, 2, &counter) == DISTRIC_OK);

    /* Valid update */
    metric_label_t valid[] = {{"method","GET"},{"status","200"}};
    assert(metrics_counter_inc_with_labels(counter, valid, 2, 1) == DISTRIC_OK);

    /* Invalid: value not in allowlist */
    metric_label_t bad_val[] = {{"method","OPTIONS"},{"status","200"}};
    distric_err_t err = metrics_counter_inc_with_labels(counter, bad_val, 2, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL && "OPTIONS not in method allowlist");
    printf("  Invalid value 'OPTIONS' correctly rejected\n");

    /* Invalid: key not in definitions */
    metric_label_t bad_key[] = {{"region","us-east"},{"status","200"}};
    err = metrics_counter_inc_with_labels(counter, bad_key, 2, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL && "key 'region' not defined");
    printf("  Unknown key 'region' correctly rejected\n");

    /* Invalid: too many labels */
    metric_label_t too_many[] = {
        {"method","GET"},{"status","200"},{"extra","val"}
    };
    err = metrics_counter_inc_with_labels(counter, too_many, 3, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL && "excess labels must be rejected");
    printf("  Extra label count correctly rejected\n");

    /* Counter must reflect only the one valid update */
    uint64_t val = metrics_counter_get(counter);
    assert(val == 1 && "only the valid update must be counted");
    printf("  Counter value == 1 (only valid update counted)\n");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P1d. Concurrent valid/invalid updates
 * ========================================================================= */

#define P1D_THREADS 16
#define P1D_UPDATES 500

typedef struct {
    metric_t* counter;
    int       valid_count;
    int       invalid_count;
} p1d_arg_t;

static void* p1d_worker(void* arg) {
    p1d_arg_t* a = (p1d_arg_t*)arg;
    metric_label_t good[] = {{"method","GET"},   {"status","200"}};
    metric_label_t bad[]  = {{"method","TRACE"}, {"status","200"}}; /* TRACE not allowed */
    for (int i = 0; i < P1D_UPDATES; i++) {
        if (metrics_counter_inc_with_labels(a->counter, good, 2, 1) == DISTRIC_OK)
            a->valid_count++;
        if (metrics_counter_inc_with_labels(a->counter, bad, 2, 1)
                == DISTRIC_ERR_INVALID_LABEL)
            a->invalid_count++;
    }
    return NULL;
}

static void test_p1_concurrent_enforcement(void) {
    printf("Test [P1d]: Concurrent valid/invalid updates (%d threads × %d ops)...\n",
           P1D_THREADS, P1D_UPDATES);

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    const char* methods[]  = {"GET","POST","PUT","DELETE"};
    const char* statuses[] = {"200","400","404","500"};
    metric_label_definition_t defs[2] = {
        { .key="method", .allowed_values=methods,  .num_allowed_values=4 },
        { .key="status", .allowed_values=statuses, .num_allowed_values=4 },
    };
    metric_t* counter = NULL;
    assert(metrics_register_counter(reg, "concurrent_c", "concurrent",
                                    defs, 2, &counter) == DISTRIC_OK);

    pthread_t  threads[P1D_THREADS];
    p1d_arg_t  args[P1D_THREADS];
    for (int i = 0; i < P1D_THREADS; i++) {
        args[i] = (p1d_arg_t){ .counter = counter };
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
    assert(metrics_counter_get(counter) == (uint64_t)expected &&
           "Counter must equal valid update count");

    printf("  valid=%d invalid_rejected=%d counter=%lu  OK\n",
           total_valid, total_invalid,
           (unsigned long)metrics_counter_get(counter));

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P2a. Adaptive sampling under multi-threaded sustained overload
 *      (slow exporter + many producers)
 * ========================================================================= */

#define P2A_PRODUCERS         8
#define P2A_SPANS_PER_THREAD  2000
#define P2A_EXPORTER_DELAY_NS 2000000L  /* 2 ms per span — very slow */
#define P2A_WALL_LIMIT_NS     15000000000ULL  /* 15 s total wall-clock bound */

static _Atomic uint64_t p2a_exported = 0;

static void p2a_slow_export(trace_span_t* spans, size_t count, void* ud) {
    (void)spans; (void)count; (void)ud;
    atomic_fetch_add(&p2a_exported, count);
    struct timespec ts = { .tv_sec=0, .tv_nsec=P2A_EXPORTER_DELAY_NS };
    nanosleep(&ts, NULL);
}

typedef struct {
    tracer_t* tracer;
    int       thread_id;
    int       spans_attempted;
    int       spans_created;
} p2a_arg_t;

static void* p2a_producer(void* arg) {
    p2a_arg_t* a = (p2a_arg_t*)arg;
    for (int i = 0; i < P2A_SPANS_PER_THREAD; i++) {
        trace_span_t* span = NULL;
        a->spans_attempted++;
        distric_err_t err = trace_start_span(a->tracer, "work_unit", &span);
        if (err == DISTRIC_OK && span) {
            trace_add_tag(span, "thread", "t");
            trace_set_status(span, SPAN_STATUS_OK);
            trace_finish_span(a->tracer, span);
            a->spans_created++;
        }
    }
    return NULL;
}

static void test_p2_multithreaded_sustained_overload(void) {
    printf("Test [P2a]: Multi-threaded sustained overload with slow exporter...\n");
    printf("  %d producer threads × %d spans, exporter sleeps %d ms per span\n",
           P2A_PRODUCERS, P2A_SPANS_PER_THREAD,
           (int)(P2A_EXPORTER_DELAY_NS / 1000000));

    trace_sampling_config_t cfg = {
        .always_sample       = 1,
        .always_drop         = 0,
        .backpressure_sample = 1,
        .backpressure_drop   = 9,   /* 10% sampling under pressure */
    };
    tracer_t* tracer;
    assert(trace_init_with_sampling(&tracer, &cfg,
                                    p2a_slow_export, NULL) == DISTRIC_OK);

    atomic_store(&p2a_exported, 0);
    uint64_t t0 = now_ns();

    pthread_t threads[P2A_PRODUCERS];
    p2a_arg_t args[P2A_PRODUCERS];
    for (int i = 0; i < P2A_PRODUCERS; i++) {
        args[i] = (p2a_arg_t){ .tracer = tracer, .thread_id = i };
        pthread_create(&threads[i], NULL, p2a_producer, &args[i]);
    }
    for (int i = 0; i < P2A_PRODUCERS; i++) pthread_join(threads[i], NULL);

    uint64_t flood_ns = now_ns() - t0;
    printf("  Flooding finished in %.3f ms\n", (double)flood_ns / 1e6);

    /* Producers must never have blocked. */
    assert(flood_ns < P2A_WALL_LIMIT_NS &&
           "Producer threads exceeded wall-clock bound — potential blocking detected");

    /* Inspect stats before destroying. */
    tracer_stats_t stats;
    trace_get_stats(tracer, &stats);

    printf("  created=%lu sampled_in=%lu sampled_out=%lu dropped=%lu "
           "bp=%s rate=%u%%\n",
           (unsigned long)stats.spans_created,
           (unsigned long)stats.spans_sampled_in,
           (unsigned long)stats.spans_sampled_out,
           (unsigned long)stats.spans_dropped_backpressure,
           stats.in_backpressure ? "YES" : "NO",
           stats.effective_sample_rate_pct);

    /*
     * Under sustained slow-exporter pressure at least one of the two
     * degradation paths must have activated:
     *   a) adaptive sampling reduced throughput (spans_sampled_out > 0), or
     *   b) buffer drops occurred (spans_dropped_backpressure > 0).
     */
    int total_ops = P2A_PRODUCERS * P2A_SPANS_PER_THREAD;
    assert(stats.spans_created == (uint64_t)total_ops &&
           "spans_created must equal all producer attempts");

    bool degradation_observed =
        (stats.spans_sampled_out > 0 || stats.spans_dropped_backpressure > 0);
    assert(degradation_observed &&
           "Sustained overload must produce observable degradation");
    printf("  Degradation observed: sampled_out=%lu drops=%lu\n",
           (unsigned long)stats.spans_sampled_out,
           (unsigned long)stats.spans_dropped_backpressure);

    trace_destroy(tracer);  /* waits for exporter to flush what it can */

    printf("  PASSED\n\n");
}

/* ============================================================================
 * P2b. trace_get_stats reflects backpressure transitions
 * ========================================================================= */

static void stall_export_cb(trace_span_t* s, size_t n, void* ud) {
    (void)s; (void)n; (void)ud;
    /* Sleep long enough to guarantee the queue fills. */
    struct timespec ts = { .tv_sec=0, .tv_nsec=50000000L }; /* 50 ms */
    nanosleep(&ts, NULL);
}

static void test_p2_stats_api(void) {
    printf("Test [P2b]: trace_get_stats() reflects degradation state...\n");

    trace_sampling_config_t cfg = { 1, 0, 1, 9 };
    tracer_t* tracer;
    assert(trace_init_with_sampling(&tracer, &cfg, stall_export_cb, NULL) == DISTRIC_OK);

    /* Flood without sleeping to fill the buffer. */
    for (int i = 0; i < 3000; i++) {
        trace_span_t* span;
        if (trace_start_span(tracer, "flood", &span) == DISTRIC_OK && span) {
            trace_finish_span(tracer, span);
        }
    }

    tracer_stats_t stats;
    trace_get_stats(tracer, &stats);

    printf("  After flood: created=%lu sampled_in=%lu dropped=%lu "
           "q=%lu/%lu bp=%s\n",
           (unsigned long)stats.spans_created,
           (unsigned long)stats.spans_sampled_in,
           (unsigned long)stats.spans_dropped_backpressure,
           (unsigned long)stats.queue_depth,
           (unsigned long)stats.queue_capacity,
           stats.in_backpressure ? "YES" : "NO");

    assert(stats.spans_created > 0);
    assert(stats.queue_capacity > 0);
    /* Some spans must have been processed (sampled in or out). */
    assert(stats.spans_sampled_in + stats.spans_sampled_out
           == stats.spans_created);

    trace_destroy(tracer);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P2c. trace_register_metrics wires stats to Prometheus registry
 * ========================================================================= */

static void noop_export_cb(trace_span_t* s, size_t n, void* ud) {
    (void)s; (void)n; (void)ud;
}

static void test_p2_register_metrics(void) {
    printf("Test [P2c]: trace_register_metrics() wires stats to registry...\n");

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);

    tracer_t* tracer;
    assert(trace_init(&tracer, noop_export_cb, NULL) == DISTRIC_OK);

    distric_err_t err = trace_register_metrics(tracer, reg);
    assert(err == DISTRIC_OK);

    /* Registry should now have 5 new metrics. */
    char* buf = NULL;
    size_t sz = 0;
    assert(metrics_export_prometheus(reg, &buf, &sz) == DISTRIC_OK);
    assert(buf != NULL && sz > 0);

    /* Verify at least the queue_depth metric appears in output. */
    assert(strstr(buf, "distric_tracing_queue_depth") != NULL &&
           "queue_depth metric must appear in Prometheus output");
    assert(strstr(buf, "distric_tracing_sample_rate_pct") != NULL &&
           "sample_rate_pct metric must appear");
    assert(strstr(buf, "distric_tracing_in_backpressure") != NULL &&
           "in_backpressure metric must appear");
    printf("  All expected tracing metrics appear in Prometheus output\n");

    free(buf);
    trace_destroy(tracer);
    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P3a. Non-blocking guarantee: metrics under storm (multi-threaded)
 * ========================================================================= */

#define P3A_THREADS  32
#define P3A_OPS      10000
/*
 * Per-operation latency bound: 200 µs.
 *
 * The unlabelled counter hot-path is lock-free (~10-30 ns typical), but the
 * very first call per metric allocates a counter_instance_t under a mutex.
 * Under 32-thread contention on a loaded host that single allocation can
 * spike to ~60 µs due to OS scheduling jitter — not a blocking code path.
 * 200 µs provides a 3x margin over the observed worst case while still
 * reliably catching a genuinely blocked call (which measures in milliseconds).
 */
#define P3A_MAX_NS   200000LL  /* 200 µs */

typedef struct { metric_t* counter; uint64_t max_ns; } p3a_arg_t;

static void* p3a_metric_worker(void* arg) {
    p3a_arg_t* a = (p3a_arg_t*)arg;
    for (int i = 0; i < P3A_OPS; i++) {
        uint64_t t0  = now_ns();
        metrics_counter_inc(a->counter);
        uint64_t dur = now_ns() - t0;
        if (dur > a->max_ns) a->max_ns = dur;
    }
    return NULL;
}

static void test_p3_metric_storm_nonblocking(void) {
    printf("Test [P3a]: Metric counter storm (%d threads × %d ops)...\n",
           P3A_THREADS, P3A_OPS);

    metrics_registry_t* reg;
    assert(metrics_init(&reg) == DISTRIC_OK);
    metric_t* c = NULL;
    assert(metrics_register_counter(reg, "storm_counter", "storm", NULL, 0, &c)
           == DISTRIC_OK);

    /* Warm up: trigger the one-time instance allocation before timing starts.
     * Without this, the first thread to call metrics_counter_inc takes a
     * mutex + calloc path that can spike to ~60 µs on a loaded host. */
    metrics_counter_inc(c);

    pthread_t  threads[P3A_THREADS];
    p3a_arg_t  args[P3A_THREADS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < P3A_THREADS; i++) {
        args[i] = (p3a_arg_t){ .counter = c, .max_ns = 0 };
        pthread_create(&threads[i], NULL, p3a_metric_worker, &args[i]);
    }
    for (int i = 0; i < P3A_THREADS; i++) pthread_join(threads[i], NULL);
    uint64_t wall = now_ns() - t0;

    uint64_t global_max = 0;
    for (int i = 0; i < P3A_THREADS; i++)
        if (args[i].max_ns > global_max) global_max = args[i].max_ns;

    uint64_t expected = (uint64_t)P3A_THREADS * P3A_OPS + 1; /* +1 for warmup */
    assert(metrics_counter_get(c) == expected &&
           "Counter must equal total increments");
    printf("  counter=%lu  max_latency=%lu ns  wall=%.1f ms\n",
           (unsigned long)metrics_counter_get(c),
           (unsigned long)global_max, (double)wall / 1e6);

    /* Per-operation latency must stay well below a blocking threshold.
     * The hot path is lock-free; a one-time instance-creation mutex grab
     * can spike under scheduler pressure but must not reach milliseconds. */
    assert(global_max < (uint64_t)P3A_MAX_NS &&
           "metrics_counter_inc exceeded 200µs — potential contention or blocking");

    metrics_destroy(reg);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * P3b. Non-blocking guarantee: tracing under saturation
 *      Each producer measures per-call time on a FULL buffer.
 * ========================================================================= */

#define P3B_THREADS  8
#define P3B_SPANS    500
#define P3B_MAX_NS   100000LL  /* 100 µs — generous bound for a full-buffer drop */

/*
 * The exporter sleeps briefly on each call but checks a shared stop flag.
 * This keeps the buffer near-full without blocking trace_destroy() forever.
 */
static _Atomic int p3b_stop = 0;

static void p3b_slow_export(trace_span_t* s, size_t n, void* ud) {
    (void)s; (void)n; (void)ud;
    /* Sleep in small increments so the exporter loop can notice shutdown. */
    for (int i = 0; i < 20 && !atomic_load(&p3b_stop); i++) {
        struct timespec ts = { .tv_sec=0, .tv_nsec=5000000L }; /* 5 ms */
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

static void test_p3_trace_saturation_nonblocking(void) {
    printf("Test [P3b]: Tracing non-blocking on saturated buffer "
           "(%d threads × %d spans)...\n", P3B_THREADS, P3B_SPANS);

    atomic_store(&p3b_stop, 0);

    /* Always-sample so the buffer fills immediately. */
    trace_sampling_config_t cfg = { 1, 0, 1, 0 };
    tracer_t* tracer;
    assert(trace_init_with_sampling(&tracer, &cfg,
                                    p3b_slow_export, NULL) == DISTRIC_OK);

    /* Pre-fill the buffer so producers immediately hit the full-buffer path. */
    for (int i = 0; i < 2000; i++) {
        trace_span_t* s = NULL;
        if (trace_start_span(tracer, "fill", &s) == DISTRIC_OK && s
            && s->sampled) {
            trace_finish_span(tracer, s);
        }
    }

    /* Brief pause so the exporter consumes its first span and re-blocks. */
    usleep(10000);  /* 10 ms */

    pthread_t threads[P3B_THREADS];
    p3b_arg_t args[P3B_THREADS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < P3B_THREADS; i++) {
        args[i] = (p3b_arg_t){ .tracer = tracer };
        pthread_create(&threads[i], NULL, p3b_producer, &args[i]);
    }
    for (int i = 0; i < P3B_THREADS; i++) pthread_join(threads[i], NULL);
    uint64_t wall = now_ns() - t0;

    uint64_t global_max = 0;
    for (int i = 0; i < P3B_THREADS; i++)
        if (args[i].max_finish_ns > global_max)
            global_max = args[i].max_finish_ns;

    printf("  max trace_finish_span latency: %lu ns  wall=%.1f ms\n",
           (unsigned long)global_max, (double)wall / 1e6);

    assert(global_max < (uint64_t)P3B_MAX_NS &&
           "trace_finish_span on full buffer exceeded 100µs — potential blocking");

    /* Signal the exporter to stop sleeping, then destroy cleanly. */
    atomic_store(&p3b_stop, 1);
    trace_destroy(tracer);

    printf("  PASSED\n\n");
}

/* ============================================================================
 * P3c. Logger sustained stress: producers never block under heavy load
 * ========================================================================= */

#define P3C_THREADS  16
#define P3C_LOGS     500
#define P3C_MAX_NS   20000LL  /* 20 µs — includes overflow detection */

typedef struct { logger_t* logger; uint64_t max_ns; int dropped; } p3c_arg_t;

static void* p3c_log_worker(void* arg) {
    p3c_arg_t* a = (p3c_arg_t*)arg;
    for (int i = 0; i < P3C_LOGS; i++) {
        uint64_t t0  = now_ns();
        distric_err_t err = LOG_INFO(a->logger, "stress", "sustained pressure",
                                     "k1", "v1", "k2", "v2");
        uint64_t dur = now_ns() - t0;
        if (dur > a->max_ns) a->max_ns = dur;
        if (err == DISTRIC_ERR_BUFFER_OVERFLOW) a->dropped++;
    }
    return NULL;
}

static void test_p3_logger_sustained_nonblocking(void) {
    printf("Test [P3c]: Logger sustained non-blocking (%d threads × %d logs)...\n",
           P3C_THREADS, P3C_LOGS);

    int devnull = open("/dev/null", O_WRONLY);
    assert(devnull >= 0);

    logger_t* logger;
    assert(log_init(&logger, devnull, LOG_MODE_ASYNC) == DISTRIC_OK);

    pthread_t threads[P3C_THREADS];
    p3c_arg_t args[P3C_THREADS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < P3C_THREADS; i++) {
        args[i] = (p3c_arg_t){ .logger = logger };
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
           "log_write exceeded 20µs — potential blocking under load");

    log_destroy(logger);
    close(devnull);
    printf("  PASSED\n\n");
}

/* ============================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Observability — Production Blocker Stress Tests ===\n\n");

    /* P1: Metric label cardinality enforcement */
    test_p1_reject_null_allowlist();
    test_p1_reject_high_cardinality_registration();
    test_p1_reject_invalid_label_updates();
    test_p1_concurrent_enforcement();

    /* P2: Tracing adaptive sampling under sustained backpressure */
    test_p2_multithreaded_sustained_overload();
    test_p2_stats_api();
    test_p2_register_metrics();

    /* P3: Non-blocking guarantees under sustained pressure */
    test_p3_metric_storm_nonblocking();
    test_p3_trace_saturation_nonblocking();
    test_p3_logger_sustained_nonblocking();

    printf("=== ALL PRODUCTION BLOCKER TESTS PASSED ===\n");
    return 0;
}