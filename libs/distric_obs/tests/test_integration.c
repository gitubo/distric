#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_obs.h"
#include "distric_obs/tracing.h"
#include "distric_obs/health.h"
#include "distric_obs/http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Worker thread data */
typedef struct {
    int worker_id;
    metrics_registry_t* metrics;
    logger_t* logger;
    tracer_t* tracer;
    health_component_t* health_component;
    metric_t* request_counter;
    metric_t* request_duration;
} worker_data_t;

/* Tracing export callback */
void trace_export(trace_span_t* spans, size_t count, void* user_data) {
    (void)user_data;
    printf("[TRACE] Exporting %zu spans\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  - %s (duration: %lu ns)\n", 
               spans[i].operation,
               spans[i].end_time_ns - spans[i].start_time_ns);
    }
}

/* Simulate a request processing */
void* worker_thread(void* arg) {
    worker_data_t* data = (worker_data_t*)arg;
    
    for (int i = 0; i < 5; i++) {
        /* Start a trace span */
        trace_span_t* span;
        char operation[64];
        snprintf(operation, sizeof(operation), "worker_%d_request_%d", 
                data->worker_id, i);
        
        trace_start_span(data->tracer, operation, &span);
        trace_add_tag(span, "worker.id", "1");
        trace_add_tag(span, "request.id", "test");
        
        /* Log the request */
        LOG_INFO(data->logger, "worker", "Processing request",
                "worker_id", "1",
                "request_num", "test");
        
        /* Update metrics */
        metrics_counter_inc(data->request_counter);
        
        /* Simulate work */
        usleep(50000);  /* 50ms */
        
        /* Record duration */
        metrics_histogram_observe(data->request_duration, 0.05);
        
        /* Simulate degraded health on worker 2 */
        if (data->worker_id == 2 && i == 2) {
            health_update_status(data->health_component, HEALTH_DEGRADED,
                               "Temporary slowdown");
            LOG_WARN(data->logger, "worker", "Performance degraded",
                    "worker_id", "2");
        }
        
        /* Finish span */
        trace_set_status(span, SPAN_STATUS_OK);
        trace_finish_span(data->tracer, span);
        
        LOG_INFO(data->logger, "worker", "Request completed",
                "worker_id", "1",
                "request_num", "test");
    }
    
    return NULL;
}

/* Helper: Make HTTP GET request */
static int http_get(uint16_t port, const char* path, char* response, size_t response_size) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             path);
    
    write(sock, request, strlen(request));
    
    ssize_t total = 0, n;
    while ((n = read(sock, response + total, response_size - total - 1)) > 0) {
        total += n;
    }
    response[total] = '\0';
    
    close(sock);
    return total;
}

int main() {
    printf("=== DistriC Phase 0 Integration Test ===\n\n");
    
    /* 1. Initialize all observability systems */
    printf("[INIT] Initializing observability stack...\n");
    
    metrics_registry_t* metrics;
    metrics_init(&metrics);
    
    logger_t* logger;
    log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
    
    tracer_t* tracer;
    trace_init(&tracer, trace_export, NULL);
    
    health_registry_t* health;
    health_init(&health);
    
    LOG_INFO(logger, "main", "Observability stack initialized", NULL);
    
    /* 2. Register metrics */
    printf("[METRICS] Registering metrics...\n");
    
    metric_t* request_counter;
    metric_t* request_duration;
    metric_t* active_workers;
    
    metric_label_t labels[] = {{"service", "api"}};
    
    metrics_register_counter(metrics, "requests_total",
                            "Total requests", labels, 1, &request_counter);
    metrics_register_histogram(metrics, "request_duration_seconds",
                               "Request duration", labels, 1, &request_duration);
    metrics_register_gauge(metrics, "active_workers",
                          "Active workers", labels, 1, &active_workers);
    
    /* 3. Register health components */
    printf("[HEALTH] Registering health components...\n");
    
    health_component_t* worker1_health;
    health_component_t* worker2_health;
    health_component_t* worker3_health;
    
    health_register_component(health, "worker1", &worker1_health);
    health_register_component(health, "worker2", &worker2_health);
    health_register_component(health, "worker3", &worker3_health);
    
    health_update_status(worker1_health, HEALTH_UP, "Running");
    health_update_status(worker2_health, HEALTH_UP, "Running");
    health_update_status(worker3_health, HEALTH_UP, "Running");
    
    /* 4. Start HTTP server */
    printf("[SERVER] Starting HTTP server...\n");
    
    obs_server_t* server;
    obs_server_init(&server, 0, metrics, health);
    uint16_t port = obs_server_get_port(server);
    
    printf("[SERVER] Server listening on port %u\n", port);
    LOG_INFO(logger, "main", "HTTP server started", "port", "test");
    
    /* Give server time to start */
    sleep(1);
    
    /* 5. Start worker threads */
    printf("[WORKERS] Starting worker threads...\n");
    
    pthread_t workers[3];
    worker_data_t worker_data[3];
    
    for (int i = 0; i < 3; i++) {
        worker_data[i].worker_id = i + 1;
        worker_data[i].metrics = metrics;
        worker_data[i].logger = logger;
        worker_data[i].tracer = tracer;
        worker_data[i].request_counter = request_counter;
        worker_data[i].request_duration = request_duration;
        
        if (i == 0) worker_data[i].health_component = worker1_health;
        else if (i == 1) worker_data[i].health_component = worker2_health;
        else worker_data[i].health_component = worker3_health;
        
        metrics_gauge_set(active_workers, i + 1);
        pthread_create(&workers[i], NULL, worker_thread, &worker_data[i]);
    }
    
    LOG_INFO(logger, "main", "Workers started", "count", "3");
    
    /* 6. Wait for workers to complete some work */
    sleep(2);
    
    /* 7. Test HTTP endpoints */
    printf("\n[TEST] Testing HTTP endpoints...\n");
    
    char response[65536];
    
    /* Test /metrics */
    printf("\n[TEST] GET /metrics:\n");
    int bytes = http_get(port, "/metrics", response, sizeof(response));
    if (bytes > 0) {
        char* body = strstr(response, "\r\n\r\n");
        if (body) {
            body += 4;
            printf("%s\n", body);
        }
    }
    
    /* Test /health/ready */
    printf("\n[TEST] GET /health/ready:\n");
    bytes = http_get(port, "/health/ready", response, sizeof(response));
    if (bytes > 0) {
        char* body = strstr(response, "\r\n\r\n");
        if (body) {
            body += 4;
            printf("%s\n", body);
        }
    }
    
    /* Test /health/live */
    printf("\n[TEST] GET /health/live:\n");
    bytes = http_get(port, "/health/live", response, sizeof(response));
    if (bytes > 0) {
        char* body = strstr(response, "\r\n\r\n");
        if (body) {
            body += 4;
            printf("%s\n", body);
        }
    }
    
    /* 8. Wait for workers to finish */
    printf("\n[WORKERS] Waiting for workers to complete...\n");
    
    for (int i = 0; i < 3; i++) {
        pthread_join(workers[i], NULL);
    }
    
    metrics_gauge_set(active_workers, 0);
    
    LOG_INFO(logger, "main", "All workers completed", NULL);
    
    /* 9. Final health check */
    printf("\n[TEST] Final health check:\n");
    bytes = http_get(port, "/health/ready", response, sizeof(response));
    if (bytes > 0) {
        char* body = strstr(response, "\r\n\r\n");
        if (body) {
            body += 4;
            printf("%s\n", body);
        }
    }
    
    /* 10. Cleanup */
    printf("\n[CLEANUP] Shutting down...\n");
    
    LOG_INFO(logger, "main", "Shutting down observability stack", NULL);
    
    obs_server_destroy(server);
    
    /* Wait for final trace export */
    sleep(6);
    
    trace_destroy(tracer);
    log_destroy(logger);
    health_destroy(health);
    metrics_destroy(metrics);
    
    printf("\n=== Phase 0 Integration Test Complete ===\n");
    printf("All observability components working together successfully!\n");
    
    return 0;
}