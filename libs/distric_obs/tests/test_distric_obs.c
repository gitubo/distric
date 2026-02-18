#define _DEFAULT_SOURCE
#include "distric_obs.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#define NUM_WORKERS 3
#define REQUESTS_PER_WORKER 10

static metrics_registry_t* metrics;
static logger_t*           logger;
static metric_t*           requests_total;
static metric_t*           request_duration;
static metric_t*           active_connections;

void* worker_thread(void* arg) {
    int worker_id = *(int*)arg;

    for (int i = 0; i < REQUESTS_PER_WORKER; i++) {
        metrics_gauge_set(active_connections, worker_id + 1);

        LOG_INFO(logger, "worker", "Processing request",
                "worker_id", "test",
                "request_id", "test", NULL);

        usleep(10000);

        metrics_counter_inc(requests_total);
        metrics_histogram_observe(request_duration, 0.010 + (i * 0.001));

        LOG_INFO(logger, "worker", "Request completed",
                "worker_id", "test",
                "duration_ms", "10", NULL);
    }

    return NULL;
}

int main() {
    printf("=== DistriC Observability Integration Test ===\n\n");

    metrics_init(&metrics);

    metrics_register_counter(metrics, "http_requests_total",
                             "Total HTTP requests", NULL, 0,
                             &requests_total);

    metrics_register_histogram(metrics, "http_request_duration_seconds",
                               "HTTP request duration", NULL, 0,
                               &request_duration);

    metrics_register_gauge(metrics, "http_active_connections",
                          "Active HTTP connections", NULL, 0,
                          &active_connections);

    log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);

    LOG_INFO(logger, "main", "Application starting",
            "version", "1.0.0",
            "environment", "production", NULL);

    pthread_t threads[NUM_WORKERS];
    int thread_ids[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        thread_ids[i] = i + 1;
        pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);

    LOG_INFO(logger, "main", "All workers completed", NULL);

    char* prometheus_output;
    size_t output_size;
    metrics_export_prometheus(metrics, &prometheus_output, &output_size);

    printf("\n=== Prometheus Metrics ===\n");
    printf("%s\n", prometheus_output);
    free(prometheus_output);

    log_destroy(logger);
    metrics_destroy(metrics);

    printf("=== Integration test complete ===\n");
    return 0;
}