#include "distric_obs.h"
#include "distric_obs/metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

#define NUM_THREADS 100
#define INCREMENTS_PER_THREAD 1000

static metric_t* shared_counter = NULL;

/* Thread worker that increments shared counter */
void* counter_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < INCREMENTS_PER_THREAD; i++) {
        metrics_counter_inc(shared_counter);
    }
    return NULL;
}

/* Test concurrent counter updates */
void test_concurrent_counter() {
    printf("Test: Concurrent counter updates...\n");
    
    metrics_registry_t* registry;
    distric_err_t err = metrics_init(&registry);
    assert(err == DISTRIC_OK);
    
    err = metrics_register_counter(registry, "test_counter",
                                   "Test concurrent updates", NULL, 0,
                                   &shared_counter);
    assert(err == DISTRIC_OK);
    
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, counter_thread, NULL);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Verify final count */
    uint64_t expected = NUM_THREADS * INCREMENTS_PER_THREAD;
    uint64_t actual = atomic_load(&shared_counter->data.counter.value);
    
    printf("  Expected: %lu, Actual: %lu\n", expected, actual);
    assert(actual == expected);
    
    metrics_destroy(registry);
    printf("  PASSED\n\n");
}

/* Test gauge operations */
void test_gauge() {
    printf("Test: Gauge operations...\n");
    
    metrics_registry_t* registry;
    metric_t* gauge;
    
    distric_err_t err = metrics_init(&registry);
    assert(err == DISTRIC_OK);
    
    err = metrics_register_gauge(registry, "test_gauge", "Test gauge", NULL, 0, &gauge);
    assert(err == DISTRIC_OK);
    
    metrics_gauge_set(gauge, 42.5);
    
    uint64_t bits = atomic_load(&gauge->data.gauge.bits);
    double value;
    memcpy(&value, &bits, sizeof(double));
    
    printf("  Set value: 42.5, Got: %.1f\n", value);
    assert(value == 42.5);
    
    metrics_destroy(registry);
    printf("  PASSED\n\n");
}

/* Test histogram observations */
void test_histogram() {
    printf("Test: Histogram observations...\n");
    
    metrics_registry_t* registry;
    metric_t* histogram;
    
    distric_err_t err = metrics_init(&registry);
    assert(err == DISTRIC_OK);
    
    err = metrics_register_histogram(registry, "test_histogram", 
                                     "Test histogram", NULL, 0, &histogram);
    assert(err == DISTRIC_OK);
    
    /* Record some observations */
    metrics_histogram_observe(histogram, 0.5);
    metrics_histogram_observe(histogram, 5.0);
    metrics_histogram_observe(histogram, 50.0);
    metrics_histogram_observe(histogram, 500.0);
    
    uint64_t count = atomic_load(&histogram->data.histogram.count);
    assert(count == 4);
    
    printf("  Recorded 4 observations, count: %lu\n", count);
    
    metrics_destroy(registry);
    printf("  PASSED\n\n");
}

/* Test Prometheus export format */
void test_prometheus_export() {
    printf("Test: Prometheus export format...\n");
    
    metrics_registry_t* registry;
    metric_t* counter;
    
    distric_err_t err = metrics_init(&registry);
    assert(err == DISTRIC_OK);
    
    metric_label_t labels[] = {
        {"method", "GET"},
        {"status", "200"}
    };
    
    err = metrics_register_counter(registry, "http_requests_total",
                                   "Total HTTP requests", labels, 2, &counter);
    assert(err == DISTRIC_OK);
    
    metrics_counter_add(counter, 42);
    
    char* output;
    size_t output_size;
    err = metrics_export_prometheus(registry, &output, &output_size);
    assert(err == DISTRIC_OK);
    
    printf("  Prometheus output:\n%s\n", output);
    
    /* Verify format contains expected strings */
    assert(strstr(output, "# HELP http_requests_total") != NULL);
    assert(strstr(output, "# TYPE http_requests_total counter") != NULL);
    assert(strstr(output, "method=\"GET\"") != NULL);
    assert(strstr(output, "status=\"200\"") != NULL);
    assert(strstr(output, "42") != NULL);
    
    free(output);
    metrics_destroy(registry);
    printf("  PASSED\n\n");
}

int main() {
    printf("=== DistriC Metrics Tests ===\n\n");
    
    test_concurrent_counter();
    test_gauge();
    test_histogram();
    test_prometheus_export();
    
    printf("=== All metrics tests passed ===\n");
    return 0;
}