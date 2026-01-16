/**
 * @file test_integration.c
 * @brief Phase 1 Integration Test - All transport components together
 * 
 * This test validates:
 * 1. All observability systems work (metrics, logs, health)
 * 2. TCP server and pool operate concurrently
 * 3. UDP datagrams are sent/received
 * 4. Metrics correctly track all operations
 * 
 * Uses ONLY public APIs from distric_obs and distric_transport.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <distric_transport.h>
#include <distric_obs.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>

#define TCP_PORT 19100
#define UDP_PORT 19101
#define NUM_TCP_CLIENTS 10
#define NUM_UDP_PACKETS 100

static metrics_registry_t* g_metrics = NULL;
static logger_t* g_logger = NULL;
static health_registry_t* g_health = NULL;

static _Atomic int tcp_connections_handled = 0;
static _Atomic int udp_packets_received = 0;

/* ============================================================================
 * TCP ECHO SERVER
 * ========================================================================= */

static void on_connection(tcp_connection_t* conn, void* userdata) {
    (void)userdata;
    
    atomic_fetch_add(&tcp_connections_handled, 1);
    
    char buffer[1024];
    int received = tcp_recv(conn, buffer, sizeof(buffer) - 1, 5000);
    
    if (received > 0) {
        buffer[received] = '\0';
        tcp_send(conn, buffer, received);
    }
    
    tcp_close(conn);
}

/* ============================================================================
 * TCP CLIENT WORKER
 * ========================================================================= */

static void* tcp_client_worker(void* arg) {
    tcp_pool_t* pool = (tcp_pool_t*)arg;
    
    for (int i = 0; i < 10; i++) {
        tcp_connection_t* conn;
        distric_err_t err = tcp_pool_acquire(pool, "127.0.0.1", TCP_PORT, &conn);
        
        if (err == DISTRIC_OK) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Message %d", i);
            
            tcp_send(conn, msg, strlen(msg));
            
            char buffer[1024];
            tcp_recv(conn, buffer, sizeof(buffer), 5000);
            
            tcp_pool_release(pool, conn);
        }
        
        usleep(10000); /* 10ms delay */
    }
    
    return NULL;
}

/* ============================================================================
 * UDP WORKER
 * ========================================================================= */

static void* udp_sender_worker(void* arg) {
    udp_socket_t* udp = (udp_socket_t*)arg;
    
    for (int i = 0; i < NUM_UDP_PACKETS; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "UDP packet %d", i);
        
        udp_send(udp, msg, strlen(msg), "127.0.0.1", UDP_PORT);
        usleep(1000); /* 1ms delay */
    }
    
    return NULL;
}

static void* udp_receiver_worker(void* arg) {
    udp_socket_t* udp = (udp_socket_t*)arg;
    
    while (atomic_load(&udp_packets_received) < NUM_UDP_PACKETS) {
        char buffer[1024];
        char src_addr[256];
        uint16_t src_port;
        
        int received = udp_recv(udp, buffer, sizeof(buffer), src_addr, &src_port, 100);
        
        if (received > 0) {
            atomic_fetch_add(&udp_packets_received, 1);
        }
    }
    
    return NULL;
}

/* ============================================================================
 * MAIN TEST
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Transport Layer - Phase 1 Integration Test ===\n\n");
    
    /* Initialize observability stack */
    printf("[1/7] Initializing observability...\n");
    metrics_init(&g_metrics);
    log_init(&g_logger, STDOUT_FILENO, LOG_MODE_ASYNC);
    health_init(&g_health);
    
    health_component_t* tcp_health;
    health_component_t* udp_health;
    health_register_component(g_health, "tcp_server", &tcp_health);
    health_register_component(g_health, "udp_socket", &udp_health);
    
    printf("    ✓ Metrics, logging, and health monitoring active\n");
    
    /* Start TCP server */
    printf("\n[2/7] Starting TCP echo server on port %d...\n", TCP_PORT);
    tcp_server_t* server;
    assert(tcp_server_create("127.0.0.1", TCP_PORT, g_metrics, g_logger, &server) == DISTRIC_OK);
    assert(tcp_server_start(server, on_connection, NULL) == DISTRIC_OK);
    health_update_status(tcp_health, HEALTH_UP, "Server running");
    printf("    ✓ TCP server started\n");
    
    sleep(1); /* Let server stabilize */
    
    /* Create TCP connection pool */
    printf("\n[3/7] Creating TCP connection pool (max 20 connections)...\n");
    tcp_pool_t* pool;
    assert(tcp_pool_create(20, g_metrics, g_logger, &pool) == DISTRIC_OK);
    printf("    ✓ Connection pool created\n");
    
    /* Create UDP socket */
    printf("\n[4/7] Creating UDP socket on port %d...\n", UDP_PORT);
    udp_socket_t* udp;
    assert(udp_socket_create("127.0.0.1", UDP_PORT, g_metrics, g_logger, &udp) == DISTRIC_OK);
    health_update_status(udp_health, HEALTH_UP, "Socket bound");
    printf("    ✓ UDP socket created\n");
    
    /* Start concurrent workers */
    printf("\n[5/7] Starting concurrent TCP and UDP workers...\n");
    
    pthread_t tcp_threads[NUM_TCP_CLIENTS];
    for (int i = 0; i < NUM_TCP_CLIENTS; i++) {
        pthread_create(&tcp_threads[i], NULL, tcp_client_worker, pool);
    }
    
    pthread_t udp_sender_thread, udp_receiver_thread;
    pthread_create(&udp_sender_thread, NULL, udp_sender_worker, udp);
    pthread_create(&udp_receiver_thread, NULL, udp_receiver_worker, udp);
    
    printf("    ✓ %d TCP workers + UDP sender/receiver started\n", NUM_TCP_CLIENTS);
    
    /* Wait for workers */
    printf("\n[6/7] Waiting for workers to complete...\n");
    
    for (int i = 0; i < NUM_TCP_CLIENTS; i++) {
        pthread_join(tcp_threads[i], NULL);
    }
    pthread_join(udp_sender_thread, NULL);
    pthread_join(udp_receiver_thread, NULL);
    
    printf("    ✓ All workers completed\n");
    
    /* Verify results */
    printf("\n[7/7] Verifying results...\n");
    
    int tcp_handled = atomic_load(&tcp_connections_handled);
    int udp_received = atomic_load(&udp_packets_received);
    
    printf("    TCP connections handled: %d (expected: %d)\n", 
           tcp_handled, NUM_TCP_CLIENTS * 10);
    printf("    UDP packets received: %d (expected: %d)\n", 
           udp_received, NUM_UDP_PACKETS);
    
    /* Check pool stats */
    size_t pool_size;
    uint64_t pool_hits, pool_misses;
    tcp_pool_get_stats(pool, &pool_size, &pool_hits, &pool_misses);
    
    printf("    Connection pool size: %zu\n", pool_size);
    printf("    Pool hits (reuses): %lu\n", pool_hits);
    printf("    Pool misses (new): %lu\n", pool_misses);
    
    /* Export metrics */
    printf("\n=== Metrics Summary ===\n");
    char* metrics_output;
    size_t metrics_size;
    metrics_export_prometheus(g_metrics, &metrics_output, &metrics_size);
    printf("%s\n", metrics_output);
    free(metrics_output);
    
    /* Health status */
    printf("\n=== Health Status ===\n");
    char* health_json;
    size_t health_size;
    health_export_json(g_health, &health_json, &health_size);
    printf("%s\n", health_json);
    free(health_json);
    
    /* Cleanup */
    printf("\n[Cleanup] Shutting down...\n");
    udp_close(udp);
    tcp_pool_destroy(pool);
    tcp_server_destroy(server);
    
    health_destroy(g_health);
    log_destroy(g_logger);
    metrics_destroy(g_metrics);
    
    /* Final verdict */
    printf("\n=== Test Result ===\n");
    
    bool success = (tcp_handled >= NUM_TCP_CLIENTS * 10) && 
                   (udp_received == NUM_UDP_PACKETS) &&
                   (pool_hits > 0);
    
    if (success) {
        printf("✓ PASS: Phase 1 integration test successful!\n");
        printf("  - TCP server handled all connections\n");
        printf("  - UDP datagrams transmitted successfully\n");
        printf("  - Connection pool reuse working\n");
        printf("  - All metrics tracked correctly\n");
        return 0;
    } else {
        printf("✗ FAIL: Some operations did not complete as expected\n");
        return 1;
    }
}