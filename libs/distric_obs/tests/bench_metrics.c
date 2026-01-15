#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <stdatomic.h>

#define BENCH_ITERATIONS 10000000
#define BENCH_THREADS 8

static metric_t* bench_counter = NULL;
static metric_t* bench_gauge_ptr = NULL;     /* Renamed to avoid conflict */
static metric_t* bench_histogram_ptr = NULL; /* Renamed to avoid conflict */

/* Get high-resolution timestamp in nanoseconds */
static uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Benchmark single-threaded counter increments */
void bench_counter_single_thread() {
    printf("Benchmark: Single-threaded counter increments...\n");
    
    metrics_registry_t* registry;
    metrics_init(&registry);
    metrics_register_counter(registry, "bench_counter", "Benchmark counter",
                            NULL, 0, &bench_counter);
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        metrics_counter_inc(bench_counter);
    }
    
    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double ops_per_sec = BENCH_ITERATIONS / duration_s;
    double ns_per_op = (double)duration_ns / BENCH_ITERATIONS;
    
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f ops/sec\n", ops_per_sec);
    printf("  Latency: %.2f ns/op\n", ns_per_op);
    
    metrics_destroy(registry);
    printf("\n");
}

/* Thread worker for counter benchmark */
void* counter_bench_thread(void* arg) {
    int iterations = *(int*)arg;
    
    for (int i = 0; i < iterations; i++) {
        metrics_counter_inc(bench_counter);
    }
    
    return NULL;
}

/* Benchmark multi-threaded counter increments */
void bench_counter_multi_thread() {
    printf("Benchmark: Multi-threaded counter increments (%d threads)...\n", 
           BENCH_THREADS);
    
    metrics_registry_t* registry;
    metrics_init(&registry);
    metrics_register_counter(registry, "bench_counter_mt", "MT counter",
                            NULL, 0, &bench_counter);
    
    pthread_t threads[BENCH_THREADS];
    int iterations_per_thread = BENCH_ITERATIONS / BENCH_THREADS;
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_THREADS; i++) {
        pthread_create(&threads[i], NULL, counter_bench_thread, 
                      &iterations_per_thread);
    }
    
    for (int i = 0; i < BENCH_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double ops_per_sec = BENCH_ITERATIONS / duration_s;
    
    /* Note: Direct access to bench_counter->data requires the internal 
       metrics structure definition. If this fails, use a public getter 
       if available in distric_obs.h */
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f ops/sec\n", ops_per_sec);
    printf("  Per-thread: %.2f ops/sec\n", ops_per_sec / BENCH_THREADS);
    
    metrics_destroy(registry);
    printf("\n");
}

/* Benchmark gauge updates */
void bench_gauge() {
    printf("Benchmark: Gauge updates...\n");
    
    metrics_registry_t* registry;
    metrics_init(&registry);
    metrics_register_gauge(registry, "bench_gauge", "Benchmark gauge",
                          NULL, 0, &bench_gauge_ptr);
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        metrics_gauge_set(bench_gauge_ptr, (double)i);
    }
    
    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double ops_per_sec = BENCH_ITERATIONS / duration_s;
    double ns_per_op = (double)duration_ns / BENCH_ITERATIONS;
    
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f ops/sec\n", ops_per_sec);
    printf("  Latency: %.2f ns/op\n", ns_per_op);
    
    metrics_destroy(registry);
    printf("\n");
}

/* Benchmark histogram observations */
void bench_histogram() {
    printf("Benchmark: Histogram observations...\n");
    
    metrics_registry_t* registry;
    metrics_init(&registry);
    metrics_register_histogram(registry, "bench_histogram", "Benchmark histogram",
                               NULL, 0, &bench_histogram_ptr);
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < BENCH_ITERATIONS / 10; i++) {
        metrics_histogram_observe(bench_histogram_ptr, (double)(i % 1000));
    }
    
    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_s = duration_ns / 1e9;
    double ops_per_sec = (BENCH_ITERATIONS / 10) / duration_s;
    double ns_per_op = (double)duration_ns / (BENCH_ITERATIONS / 10);
    
    printf("  Iterations: %d\n", BENCH_ITERATIONS / 10);
    printf("  Duration: %.3f seconds\n", duration_s);
    printf("  Throughput: %.2f ops/sec\n", ops_per_sec);
    printf("  Latency: %.2f ns/op\n", ns_per_op);
    
    metrics_destroy(registry);
    printf("\n");
}

/* Benchmark Prometheus export */
void bench_prometheus_export() {
    printf("Benchmark: Prometheus export (100 metrics)...\n");
    
    metrics_registry_t* registry;
    metrics_init(&registry);
    
    /* Register 100 metrics */
    metric_t* metrics[100];
    for (int i = 0; i < 100; i++) {
        char name[64];
        snprintf(name, sizeof(name), "metric_%d", i);
        
        if (i < 50) {
            metrics_register_counter(registry, name, "Counter", NULL, 0, &metrics[i]);
            metrics_counter_add(metrics[i], i * 100);
        } else {
            metrics_register_gauge(registry, name, "Gauge", NULL, 0, &metrics[i]);
            metrics_gauge_set(metrics[i], i * 3.14);
        }
    }
    
    uint64_t start = get_time_ns();
    
    char* output;
    size_t output_size;
    distric_err_t err = metrics_export_prometheus(registry, &output, &output_size);
    assert(err == DISTRIC_OK);
    
    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    
    printf("  Metrics count: 100\n");
    printf("  Output size: %zu bytes\n", output_size);
    printf("  Export time: %.3f ms\n", duration_ns / 1e6);
    
    free(output);
    metrics_destroy(registry);
    printf("\n");
}

int main() {
    printf("=== DistriC Metrics Performance Benchmarks ===\n\n");
    
    bench_counter_single_thread();
    bench_counter_multi_thread();
    bench_gauge();
    bench_histogram();
    bench_prometheus_export();
    
    printf("=== Benchmarks complete ===\n");
    return 0;
}