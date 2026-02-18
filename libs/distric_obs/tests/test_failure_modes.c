/*
 * test_failure_modes.c — DistriC Observability — Failure Mode & Chaos Tests
 *
 * Covers:
 *   FM1. Ring buffer saturation — log_write must never block; drops are counted.
 *   FM2. Concurrent logger shutdown — no UAF, no hang.
 *   FM3. Span buffer saturation — trace_finish_span never blocks.
 *   FM4. Metrics registry full — register past limit fails gracefully.
 *   FM5. Metrics cardinality enforcement — unbounded label dimension rejected.
 *   FM6. Config validation — invalid configs fail fast.
 *   FM7. Concurrent producers + stalled exporter — adaptive sampling engages.
 *   FM8. Safe logging API (log_write_kv) — no NULL sentinel needed.
 *   FM9. log_register_metrics / trace_register_metrics — backpressure gauges visible.
 *   FM10. Lifecycle: double-retain/release; retain after implicit-destroy detected.
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
 * Utilities
 * ========================================================================= */

#define PASS(name) printf("  [PASS] %s\n", name)
#define FAIL(name, msg) do { \
    fprintf(stderr, "  [FAIL] %s: %s\n", name, msg); abort(); } while(0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * FM1: Ring buffer saturation — writes never block
 * ========================================================================= */

void test_fm1_ring_buffer_saturation(void) {
    printf("FM1: Ring buffer saturation...\n");

    /* Use a small ring (capacity = 16) */
    logging_config_t cfg = {
        .fd                   = STDERR_FILENO,
        .mode                 = LOG_MODE_ASYNC,
        .ring_buffer_capacity = 16,
    };
    logger_t* logger;
    assert(log_init_with_config(&logger, &cfg) == DISTRIC_OK);

    /* Flood far more than capacity */
    int overflows = 0;
    uint64_t t0 = now_ns();
    for (int i = 0; i < 100000; i++) {
        distric_err_t err =
            log_write(logger, LOG_LEVEL_INFO, "test", "saturate", NULL);
        if (err == DISTRIC_ERR_BUFFER_OVERFLOW) overflows++;
    }
    uint64_t elapsed = now_ns() - t0;

    /* Must complete in < 5 seconds (should be milliseconds) */
    assert(elapsed < 5000000000ULL);
    /* Must have seen overflows */
    assert(overflows > 0);

    printf("  overflows=%d, elapsed=%.3fms\n",
           overflows, (double)elapsed / 1e6);
    log_destroy(logger);
    PASS("FM1");
}

/* ============================================================================
 * FM2: Concurrent shutdown — no hang, no crash
 * ========================================================================= */

typedef struct {
    logger_t* logger;
    _Atomic int stop;
} fm2_state_t;

static void* fm2_producer(void* arg) {
    fm2_state_t* s = (fm2_state_t*)arg;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        log_write(s->logger, LOG_LEVEL_INFO, "fm2", "msg", NULL);
    }
    return NULL;
}

void test_fm2_concurrent_shutdown(void) {
    printf("FM2: Concurrent logger shutdown...\n");

    logging_config_t cfg = {
        .fd                   = STDERR_FILENO,
        .mode                 = LOG_MODE_ASYNC,
        .ring_buffer_capacity = 64,
    };
    logger_t* logger;
    assert(log_init_with_config(&logger, &cfg) == DISTRIC_OK);

    fm2_state_t state = { .logger = logger };
    atomic_init(&state.stop, 0);

    pthread_t producers[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&producers[i], NULL, fm2_producer, &state);

    /* Let producers run briefly */
    struct timespec ts = { 0, 50000000 }; /* 50ms */
    nanosleep(&ts, NULL);

    /* Signal producers to stop */
    atomic_store_explicit(&state.stop, 1, memory_order_relaxed);
    for (int i = 0; i < 4; i++)
        pthread_join(producers[i], NULL);

    /* Destroy while potentially still writing — must not hang */
    uint64_t t0 = now_ns();
    log_destroy(logger);
    uint64_t dt = now_ns() - t0;
    assert(dt < 3000000000ULL); /* < 3s */

    PASS("FM2");
}

/* ============================================================================
 * FM3: Span buffer saturation — finish never blocks
 * ========================================================================= */

static void null_exporter(trace_span_t* spans, size_t count, void* ud) {
    (void)spans; (void)count; (void)ud;
}

static void stall_exporter(trace_span_t* spans, size_t count, void* ud) {
    (void)spans; (void)count;
    /* Simulate a slow exporter */
    uint32_t* stall_ms = (uint32_t*)ud;
    struct timespec ts = { *stall_ms / 1000, (*stall_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void test_fm3_span_buffer_saturation(void) {
    printf("FM3: Span buffer saturation...\n");

    uint32_t stall_ms = 200;
    tracer_config_t cfg = {
        .sampling         = { .always_sample = 1, .always_drop = 0,
                              .backpressure_sample = 1, .backpressure_drop = 9 },
        .buffer_capacity  = 16,
        .export_interval_ms = 100,
        .export_callback  = stall_exporter,
        .user_data        = &stall_ms,
    };
    tracer_t* tracer;
    assert(trace_init_with_config(&tracer, &cfg) == DISTRIC_OK);

    int drops = 0, ok = 0;
    uint64_t t0 = now_ns();

    for (int i = 0; i < 10000; i++) {
        trace_span_t* span;
        distric_err_t err = trace_start_span(tracer, "flood", &span);
        if (err == DISTRIC_ERR_BACKPRESSURE) { drops++; continue; }
        if (err == DISTRIC_OK) {
            err = trace_finish_span(tracer, span);
            ok++;
        }
    }

    uint64_t elapsed = now_ns() - t0;
    /* Must complete well under 5 seconds */
    assert(elapsed < 5000000000ULL);
    printf("  ok=%d drops=%d elapsed=%.3fms\n",
           ok, drops, (double)elapsed / 1e6);

    trace_destroy(tracer);
    PASS("FM3");
}

/* ============================================================================
 * FM4: Metrics registry full
 * ========================================================================= */

void test_fm4_registry_full(void) {
    printf("FM4: Metrics registry full...\n");

    metrics_config_t cfg = { .max_metrics = 4 };
    metrics_registry_t* registry;
    assert(metrics_init_with_config(&registry, &cfg) == DISTRIC_OK);

    metric_t* m;
    assert(metrics_register_counter(registry, "a", "a", NULL, 0, &m) == DISTRIC_OK);
    assert(metrics_register_counter(registry, "b", "b", NULL, 0, &m) == DISTRIC_OK);
    assert(metrics_register_counter(registry, "c", "c", NULL, 0, &m) == DISTRIC_OK);
    assert(metrics_register_counter(registry, "d", "d", NULL, 0, &m) == DISTRIC_OK);

    distric_err_t err = metrics_register_counter(registry, "e", "e", NULL, 0, &m);
    assert(err == DISTRIC_ERR_REGISTRY_FULL);

    metrics_destroy(registry);
    PASS("FM4");
}

/* ============================================================================
 * FM5: Label cardinality enforcement
 * ========================================================================= */

void test_fm5_cardinality_enforcement(void) {
    printf("FM5: Label cardinality enforcement...\n");

    metrics_registry_t* registry;
    assert(metrics_init(&registry) == DISTRIC_OK);

    /* NULL allowed_values → unbounded → registration must fail */
    metric_label_definition_t bad_def = {
        .key = "env", .allowed_values = NULL, .num_allowed_values = 0
    };
    metric_t* m;
    distric_err_t err =
        metrics_register_counter(registry, "bad", "bad", &bad_def, 1, &m);
    assert(err == DISTRIC_ERR_HIGH_CARDINALITY);

    /* Valid allowlist → must succeed */
    const char* envs[] = { "prod", "staging" };
    metric_label_definition_t good_def = {
        .key = "env", .allowed_values = envs, .num_allowed_values = 2
    };
    err = metrics_register_counter(registry, "good", "good", &good_def, 1, &m);
    assert(err == DISTRIC_OK);

    /* Update with unknown value → must fail */
    metric_label_t label = { .key = "env", .value = "dev" };
    err = metrics_counter_inc_labels(m, &label, 1);
    assert(err == DISTRIC_ERR_INVALID_LABEL);

    /* Update with valid value → must succeed */
    strncpy(label.value, "prod", sizeof(label.value) - 1);
    err = metrics_counter_inc_labels(m, &label, 1);
    assert(err == DISTRIC_OK);
    assert(metrics_counter_get(m) == 0);  /* unlabelled instance is 0 */

    metrics_destroy(registry);
    PASS("FM5");
}

/* ============================================================================
 * FM6: Config validation — invalid configs fail fast
 * ========================================================================= */

void test_fm6_config_validation(void) {
    printf("FM6: Config validation...\n");

    /* Logger with invalid fd */
    logging_config_t bad_cfg = { .fd = -1, .mode = LOG_MODE_ASYNC };
    logger_t* logger;
    assert(log_init_with_config(&logger, &bad_cfg) != DISTRIC_OK);

    /* Tracer with no callback */
    tracer_config_t bad_tcfg = { .export_callback = NULL };
    tracer_t* tracer;
    assert(trace_init_with_config(&tracer, &bad_tcfg) != DISTRIC_OK);

    /* NULL config pointers */
    assert(metrics_init_with_config(NULL, NULL) != DISTRIC_OK);
    assert(log_init_with_config(NULL, NULL)      != DISTRIC_OK);
    assert(trace_init_with_config(NULL, NULL)    != DISTRIC_OK);

    PASS("FM6");
}

/* ============================================================================
 * FM7: Concurrent producers with stalled exporter — adaptive sampling engages
 * ========================================================================= */

#define FM7_THREADS 8
#define FM7_SPANS_PER_THREAD 2000

typedef struct {
    tracer_t* tracer;
    int       thread_id;
    uint64_t  sampled_out;
    uint64_t  dropped;
    uint64_t  completed;
} fm7_worker_t;

static void* fm7_worker(void* arg) {
    fm7_worker_t* w = (fm7_worker_t*)arg;
    for (int i = 0; i < FM7_SPANS_PER_THREAD; i++) {
        trace_span_t* span;
        distric_err_t err = trace_start_span(w->tracer, "fm7_load", &span);
        if (err == DISTRIC_ERR_BACKPRESSURE) { w->dropped++; continue; }
        if (err == DISTRIC_OK) {
            if (span->sampled) {
                trace_finish_span(w->tracer, span);
                w->completed++;
            } else {
                w->sampled_out++;
            }
        }
    }
    return NULL;
}

void test_fm7_adaptive_sampling_under_load(void) {
    printf("FM7: Adaptive sampling under sustained producer load...\n");

    uint32_t stall_ms = 100;
    tracer_config_t cfg = {
        .sampling         = { .always_sample = 1, .always_drop = 0,
                              .backpressure_sample = 1, .backpressure_drop = 9 },
        .buffer_capacity  = 32,
        .export_interval_ms = 50,
        .export_callback  = stall_exporter,
        .user_data        = &stall_ms,
    };
    tracer_t* tracer;
    assert(trace_init_with_config(&tracer, &cfg) == DISTRIC_OK);

    fm7_worker_t workers[FM7_THREADS];
    pthread_t threads[FM7_THREADS];

    for (int i = 0; i < FM7_THREADS; i++) {
        workers[i] = (fm7_worker_t){ .tracer = tracer, .thread_id = i };
        pthread_create(&threads[i], NULL, fm7_worker, &workers[i]);
    }

    for (int i = 0; i < FM7_THREADS; i++)
        pthread_join(threads[i], NULL);

    uint64_t total_completed = 0, total_dropped = 0, total_sampled_out = 0;
    for (int i = 0; i < FM7_THREADS; i++) {
        total_completed  += workers[i].completed;
        total_dropped    += workers[i].dropped;
        total_sampled_out += workers[i].sampled_out;
    }

    tracer_stats_t stats;
    trace_get_stats(tracer, &stats);

    printf("  completed=%llu dropped=%llu sampled_out=%llu "
           "in_backpressure=%s sample_rate=%u%%\n",
           (unsigned long long)total_completed,
           (unsigned long long)total_dropped,
           (unsigned long long)total_sampled_out,
           stats.in_backpressure ? "YES" : "no",
           stats.effective_sample_rate_pct);

    /* Total accounted for must equal total attempted */
    uint64_t total = total_completed + total_dropped + total_sampled_out;
    assert(total == (uint64_t)(FM7_THREADS * FM7_SPANS_PER_THREAD));

    trace_destroy(tracer);
    PASS("FM7");
}

/* ============================================================================
 * FM8: Safe logging API — log_write_kv (Improvement #6)
 * ========================================================================= */

void test_fm8_safe_logging_api(void) {
    printf("FM8: Safe logging API (log_write_kv)...\n");

    char tmpfile[] = "/tmp/distric_fm8_XXXXXX";
    int fd = mkstemp(tmpfile);
    assert(fd >= 0);

    logger_t* logger;
    assert(log_init(&logger, fd, LOG_MODE_SYNC) == DISTRIC_OK);

    /* No NULL sentinel needed */
    log_kv_t pairs[] = {
        { "host", "localhost" },
        { "port", "5432"      },
        { "db",   "testdb"    },
    };
    distric_err_t err = log_write_kv(logger, LOG_LEVEL_INFO,
                                      "database", "Connected",
                                      pairs, 3);
    assert(err == DISTRIC_OK);

    /* Zero kv_pairs */
    err = log_write_kv(logger, LOG_LEVEL_WARN, "app", "Warning with no fields",
                       NULL, 0);
    assert(err == DISTRIC_OK);

    log_destroy(logger);
    close(fd);

    /* Verify output */
    FILE* f = fopen(tmpfile, "r");
    assert(f);
    char content[4096];
    size_t n = fread(content, 1, sizeof(content) - 1, f);
    content[n] = '\0';
    fclose(f);
    unlink(tmpfile);

    assert(strstr(content, "\"host\"") != NULL);
    assert(strstr(content, "localhost") != NULL);
    assert(strstr(content, "\"level\":\"INFO\"") != NULL);
    assert(strstr(content, "\"level\":\"WARN\"") != NULL);
    assert(strstr(content, "Warning with no fields") != NULL);

    PASS("FM8");
}

/* ============================================================================
 * FM9: Backpressure metrics visibility
 * ========================================================================= */

void test_fm9_backpressure_metrics(void) {
    printf("FM9: Backpressure metrics registration and update...\n");

    metrics_registry_t* registry;
    assert(metrics_init(&registry) == DISTRIC_OK);

    /* Logger */
    logging_config_t lcfg = {
        .fd                   = STDERR_FILENO,
        .mode                 = LOG_MODE_ASYNC,
        .ring_buffer_capacity = 16,
    };
    logger_t* logger;
    assert(log_init_with_config(&logger, &lcfg) == DISTRIC_OK);
    assert(log_register_metrics(logger, registry) == DISTRIC_OK);

    /* Double registration must fail */
    assert(log_register_metrics(logger, registry) == DISTRIC_ERR_ALREADY_EXISTS);

    /* Tracer */
    tracer_config_t tcfg = {
        .sampling         = { .always_sample = 1, .always_drop = 0,
                              .backpressure_sample = 1, .backpressure_drop = 0 },
        .buffer_capacity  = 64,
        .export_callback  = null_exporter,
    };
    tracer_t* tracer;
    assert(trace_init_with_config(&tracer, &tcfg) == DISTRIC_OK);
    assert(trace_register_metrics(tracer, registry) == DISTRIC_OK);

    /* Double registration must fail */
    assert(trace_register_metrics(tracer, registry) == DISTRIC_ERR_ALREADY_EXISTS);

    /* Flood logger to cause drops */
    for (int i = 0; i < 10000; i++)
        log_write(logger, LOG_LEVEL_INFO, "fm9", "flood", NULL);

    /* Brief sleep for background thread to update gauges */
    struct timespec ts = { 0, 200000000 };
    nanosleep(&ts, NULL);

    /* Verify prometheus output includes internal metrics */
    char* buf = NULL;
    size_t sz = 0;
    assert(metrics_export_prometheus(registry, &buf, &sz) == DISTRIC_OK);
    assert(strstr(buf, "distric_internal_log_drops_total") != NULL);
    assert(strstr(buf, "distric_internal_log_ring_fill_pct") != NULL);
    assert(strstr(buf, "distric_internal_tracer_queue_depth") != NULL);
    assert(strstr(buf, "distric_internal_tracer_sample_rate_pct") != NULL);
    free(buf);

    log_destroy(logger);
    trace_destroy(tracer);
    metrics_destroy(registry);
    PASS("FM9");
}

/* ============================================================================
 * FM10: Per-operation latency bound under saturation (Improvement #5)
 * ========================================================================= */

void test_fm10_latency_bound_under_saturation(void) {
    printf("FM10: Per-operation latency bound under saturation...\n");

    metrics_registry_t* registry;
    assert(metrics_init(&registry) == DISTRIC_OK);

    const char* vals[] = { "v" };
    metric_label_definition_t def = { .key = "k", .allowed_values = vals,
                                      .num_allowed_values = 1 };
    metric_t* counter;
    assert(metrics_register_counter(registry, "lat_test", "test", &def, 1,
                                    &counter) == DISTRIC_OK);

    metric_label_t label = { .key = "k", .value = "v" };

    /* Measure per-op latency over 1M increments */
    const int N = 1000000;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++)
        metrics_counter_inc_labels(counter, &label, 1);
    uint64_t dt = now_ns() - t0;

    double ns_per_op = (double)dt / N;
    printf("  counter_inc_labels: %.1f ns/op\n", ns_per_op);

    /* Must be < 1 microsecond per operation */
    assert(ns_per_op < 1000.0);

    metrics_destroy(registry);
    PASS("FM10");
}

/* ============================================================================
 * FM11: Metrics freeze — no registration after freeze
 * ========================================================================= */

void test_fm11_registry_freeze(void) {
    printf("FM11: Registry freeze enforcement...\n");

    metrics_registry_t* registry;
    assert(metrics_init(&registry) == DISTRIC_OK);

    metric_t* m;
    assert(metrics_register_counter(registry, "pre_freeze", "ok", NULL, 0, &m)
           == DISTRIC_OK);

    metrics_freeze(registry);

    distric_err_t err =
        metrics_register_counter(registry, "post_freeze", "fail", NULL, 0, &m);
    assert(err == DISTRIC_ERR_REGISTRY_FROZEN);

    /* Updates to existing metrics still work */
    metrics_counter_inc(m);
    assert(metrics_counter_get(m) == 1);

    metrics_destroy(registry);
    PASS("FM11");
}

/* ============================================================================
 * FM12: obs_server_init_with_config — hardened defaults
 * ========================================================================= */

void test_fm12_http_server_config(void) {
    printf("FM12: HTTP server config validation...\n");

    metrics_registry_t* registry;
    health_registry_t*  health;
    assert(metrics_init(&registry) == DISTRIC_OK);
    assert(health_init(&health)    == DISTRIC_OK);

    /* NULL metrics → must fail */
    obs_server_config_t bad = { .port = 0, .metrics = NULL, .health = health };
    obs_server_t* server;
    assert(obs_server_init_with_config(&server, &bad) != DISTRIC_OK);

    /* NULL health → must fail */
    bad.metrics = registry;
    bad.health  = NULL;
    assert(obs_server_init_with_config(&server, &bad) != DISTRIC_OK);

    /* Valid config with port=0 (auto-assign) */
    obs_server_config_t good = {
        .port             = 0,
        .metrics          = registry,
        .health           = health,
        .read_timeout_ms  = 1000,
        .write_timeout_ms = 2000,
    };
    assert(obs_server_init_with_config(&server, &good) == DISTRIC_OK);
    assert(obs_server_get_port(server) > 0);
    obs_server_destroy(server);

    health_destroy(health);
    metrics_destroy(registry);
    PASS("FM12");
}

/* ============================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Observability — Failure Mode & Chaos Tests ===\n\n");

    test_fm1_ring_buffer_saturation();
    test_fm2_concurrent_shutdown();
    test_fm3_span_buffer_saturation();
    test_fm4_registry_full();
    test_fm5_cardinality_enforcement();
    test_fm6_config_validation();
    test_fm7_adaptive_sampling_under_load();
    test_fm8_safe_logging_api();
    test_fm9_backpressure_metrics();
    test_fm10_latency_bound_under_saturation();
    test_fm11_registry_freeze();
    test_fm12_http_server_config();

    printf("\n=== All failure mode tests passed ===\n");
    return 0;
}