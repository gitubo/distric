#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_obs.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_BUFFER_SIZE 65536

static int http_get(uint16_t port, const char* path, char* response, size_t response_size) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    write(sock, request, strlen(request));

    ssize_t total = 0, n;
    while ((n = read(sock, response + total, response_size - total - 1)) > 0)
        total += n;
    response[total] = '\0';

    close(sock);
    return total;
}

void test_server_init() {
    printf("Test: HTTP server initialization...\n");

    metrics_registry_t* metrics;
    health_registry_t*  health;
    metrics_init(&metrics);
    health_init(&health);

    obs_server_t* server;
    distric_err_t err = obs_server_init(&server, 0, metrics, health);
    assert(err == DISTRIC_OK);
    assert(server != NULL);

    uint16_t port = obs_server_get_port(server);
    assert(port > 0);
    printf("  Server started on port %u\n", port);

    obs_server_destroy(server);
    health_destroy(health);
    metrics_destroy(metrics);
    printf("  PASSED\n\n");
}

void test_metrics_endpoint() {
    printf("Test: /metrics endpoint...\n");

    metrics_registry_t* metrics;
    health_registry_t*  health;
    metrics_init(&metrics);
    health_init(&health);

    metric_t* counter;
    metrics_register_counter(metrics, "test_requests_total",
                             "Test requests", NULL, 0, &counter);
    metrics_counter_add(counter, 42);

    obs_server_t* server;
    obs_server_init(&server, 0, metrics, health);
    uint16_t port = obs_server_get_port(server);
    sleep(1);

    char response[TEST_BUFFER_SIZE];
    int bytes = http_get(port, "/metrics", response, sizeof(response));
    assert(bytes > 0);
    printf("  Response (%d bytes):\n%s\n", bytes, response);

    assert(strstr(response, "200 OK")                != NULL);
    assert(strstr(response, "test_requests_total")   != NULL);
    assert(strstr(response, "42")                    != NULL);

    obs_server_destroy(server);
    health_destroy(health);
    metrics_destroy(metrics);
    printf("  PASSED\n\n");
}

void test_health_live_endpoint() {
    printf("Test: /health/live endpoint...\n");

    metrics_registry_t* metrics;
    health_registry_t*  health;
    metrics_init(&metrics);
    health_init(&health);

    obs_server_t* server;
    obs_server_init(&server, 0, metrics, health);
    uint16_t port = obs_server_get_port(server);
    sleep(1);

    char response[TEST_BUFFER_SIZE];
    int bytes = http_get(port, "/health/live", response, sizeof(response));
    assert(bytes > 0);
    printf("  Response:\n%s\n", response);

    assert(strstr(response, "200 OK")           != NULL);
    assert(strstr(response, "{\"status\":\"UP\"}") != NULL);

    obs_server_destroy(server);
    health_destroy(health);
    metrics_destroy(metrics);
    printf("  PASSED\n\n");
}

void test_health_ready_endpoint() {
    printf("Test: /health/ready endpoint...\n");

    metrics_registry_t* metrics;
    health_registry_t*  health;
    metrics_init(&metrics);
    health_init(&health);

    health_component_t* db;
    health_register_component(health, "database", &db);
    health_update_status(db, HEALTH_UP, "Connected");

    obs_server_t* server;
    obs_server_init(&server, 0, metrics, health);
    uint16_t port = obs_server_get_port(server);
    sleep(1);

    char response[TEST_BUFFER_SIZE];
    int bytes = http_get(port, "/health/ready", response, sizeof(response));
    assert(bytes > 0);
    printf("  Response:\n%s\n", response);

    assert(strstr(response, "200 OK")          != NULL);
    assert(strstr(response, "\"database\"")    != NULL);
    assert(strstr(response, "\"status\":\"UP\"") != NULL);

    obs_server_destroy(server);
    health_destroy(health);
    metrics_destroy(metrics);
    printf("  PASSED\n\n");
}

void test_not_found() {
    printf("Test: 404 Not Found...\n");

    metrics_registry_t* metrics;
    health_registry_t*  health;
    metrics_init(&metrics);
    health_init(&health);

    obs_server_t* server;
    obs_server_init(&server, 0, metrics, health);
    uint16_t port = obs_server_get_port(server);
    sleep(1);

    char response[TEST_BUFFER_SIZE];
    int bytes = http_get(port, "/invalid", response, sizeof(response));
    assert(bytes > 0);
    printf("  Response:\n%s\n", response);
    assert(strstr(response, "404 Not Found") != NULL);

    obs_server_destroy(server);
    health_destroy(health);
    metrics_destroy(metrics);
    printf("  PASSED\n\n");
}

int main() {
    printf("=== DistriC HTTP Server Tests ===\n\n");
    test_server_init();
    test_metrics_endpoint();
    test_health_live_endpoint();
    test_health_ready_endpoint();
    test_not_found();
    printf("=== All HTTP server tests passed ===\n");
    return 0;
}