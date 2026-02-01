/**
 * @file test_integration_fixed.c
 * @brief Phase 1 Integration Test - CORRECT ARCHITECTURE
 * 
 * ROOT CAUSE OF SEGFAULT:
 * The original test had a fundamental design flaw:
 * 
 * 1. Server creates connection objects when accepting
 * 2. Client pool ALSO tries to manage the SAME connection objects
 * 3. Result: Double ownership, double-free, segfault
 * 
 * PROPER ARCHITECTURE:
 * - Server-side connections: Managed by SERVER (created in accept handler)
 * - Client-side connections: Managed by POOL (created by tcp_connect)
 * 
 * These are DIFFERENT connection objects representing opposite ends of the socket.
 * They should NEVER be mixed.
 * 
 * THE FIX:
 * Don't use connection pool for echo test. Server and client each manage their own.
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
#include <errno.h>

#define TCP_PORT 19100
#define UDP_PORT 19101
#define NUM_TCP_CLIENTS 10
#define NUM_UDP_PACKETS 100

static metrics_registry_t* g_metrics = NULL;
static logger_t* g_logger = NULL;
static health_registry_t* g_health = NULL;

static _Atomic int tcp_connections_handled = 0;
static _Atomic int tcp_messages_echoed = 0;
static _Atomic int udp_packets_received = 0;
static _Atomic bool udp_receiver_running = true;

/* ============================================================================
 * TCP ECHO SERVER - Manages SERVER-SIDE connections
 * ========================================================================= */

static void on_connection(tcp_connection_t* conn, void* userdata) {
    (void)userdata;
    
    atomic_fetch_add(&tcp_connections_handled, 1);
    
    char buffer[1024];
    
    /* NON-BLOCKING recv with short timeout */
    int received = tcp_recv(conn, buffer, sizeof(buffer) - 1, 500);
    
    if (received > 0) {
        buffer[received] = '\0';
        tcp_send(conn, buffer, received);
        atomic_fetch_add(&tcp_messages_echoed, 1);
    }
    
    /* Server manages lifecycle - close when done */
    tcp_close(conn);
}

/* ============================================================================
 * TCP CLIENT WORKER - Creates OWN connections (no pool confusion)
 * ========================================================================= */

static void* tcp_client_worker(void* arg) {
    (void)arg;
    
    for (int i = 0; i < 10; i++) {
        /* FIXED: Each client creates its OWN connection */
        tcp_connection_t* conn = NULL;
        
        distric_err_t err = tcp_connect("127.0.0.1", TCP_PORT, 5000, 
                                       g_metrics, g_logger, &conn);
        
        if (err != DISTRIC_OK || !conn) {
            usleep(50000);
            continue;
        }
        
        char msg[64];
        snprintf(msg, sizeof(msg), "Message %d", i);
        
        int sent = tcp_send(conn, msg, strlen(msg));
        if (sent != (int)strlen(msg)) {
            tcp_close(conn);
            continue;
        }
        
        usleep(10000); /* Give server time to echo */
        
        char buffer[1024];
        int received = tcp_recv(conn, buffer, sizeof(buffer), 1000);
        
        /* Client manages lifecycle - close when done */
        tcp_close(conn);
        
        if (received != sent) {
            continue;
        }
        
        usleep(10000);
    }
    
    return NULL;
}

/* ============================================================================
 * POOL TEST - Separate test showing pool actually works
 * ========================================================================= */

static _Atomic int pool_test_success = 0;

static void* pool_test_worker(void* arg) {
    tcp_pool_t* pool = (tcp_pool_t*)arg;
    
    for (int i = 0; i < 5; i++) {
        tcp_connection_t* conn = NULL;
        
        /* Acquire from pool */
        distric_err_t err = tcp_pool_acquire(pool, "127.0.0.1", TCP_PORT, &conn);
        
        if (err != DISTRIC_OK || !conn) {
            usleep(50000);
            continue;
        }
        
        char msg[64];
        snprintf(msg, sizeof(msg), "Pool msg %d", i);
        
        int sent = tcp_send(conn, msg, strlen(msg));
        if (sent != (int)strlen(msg)) {
            tcp_pool_mark_failed(pool, conn);
            tcp_pool_release(pool, conn);
            continue;
        }
        
        usleep(10000);
        
        char buffer[1024];
        int received = tcp_recv(conn, buffer, sizeof(buffer), 1000);
        
        if (received == sent) {
            atomic_fetch_add(&pool_test_success, 1);
        }
        
        /* CRITICAL: Release back to pool, don't close */
        tcp_pool_release(pool, conn);
        
        usleep(10000);
    }
    
    return NULL;
}

/* ============================================================================
 * UDP WORKERS
 * ========================================================================= */

static void* udp_sender_worker(void* arg) {
    udp_socket_t* udp = (udp_socket_t*)arg;
    
    for (int i = 0; i < NUM_UDP_PACKETS; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "UDP packet %d", i);
        
        udp_send(udp, msg, strlen(msg), "127.0.0.1", UDP_PORT);
        usleep(5000);
    }
    
    return NULL;
}

static void* udp_receiver_worker(void* arg) {
    udp_socket_t* udp = (udp_socket_t*)arg;
    int consecutive_timeouts = 0;
    
    while (atomic_load(&udp_receiver_running) && consecutive_timeouts < 10) {
        char buffer[1024];
        char src_addr[256];
        uint16_t src_port;
        
        int received = udp_recv(udp, buffer, sizeof(buffer), 
                               src_addr, &src_port, 100);
        
        if (received > 0) {
            atomic_fetch_add(&udp_packets_received, 1);
            consecutive_timeouts = 0;
        } else if (received == 0) {
            consecutive_timeouts++;
        }
    }
    
    return NULL;
}

/* ============================================================================
 * MAIN TEST
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Transport Layer - Phase 1 Integration Test (FIXED) ===\n\n");
    
    /* Initialize observability */
    printf("[1/8] Initializing observability...\n");
    assert(metrics_init(&g_metrics) == DISTRIC_OK);
    assert(log_init(&g_logger, STDOUT_FILENO, LOG_MODE_ASYNC) == DISTRIC_OK);
    assert(health_init(&g_health) == DISTRIC_OK);
    
    health_component_t* tcp_health;
    health_component_t* udp_health;
    health_register_component(g_health, "tcp_server", &tcp_health);
    health_register_component(g_health, "udp_socket", &udp_health);
    printf("    ✓ Observability initialized\n");
    
    /* Start TCP server */
    printf("\n[2/8] Starting TCP echo server on port %d...\n", TCP_PORT);
    tcp_server_t* server;
    assert(tcp_server_create("127.0.0.1", TCP_PORT, g_metrics, g_logger, &server) == DISTRIC_OK);
    assert(tcp_server_start(server, on_connection, NULL) == DISTRIC_OK);
    health_update_status(tcp_health, HEALTH_UP, "Server running");
    printf("    ✓ TCP server started\n");
    
    sleep(1);
    
    /* Create UDP socket */
    printf("\n[3/8] Creating UDP socket on port %d...\n", UDP_PORT);
    udp_socket_t* udp;
    assert(udp_socket_create("127.0.0.1", UDP_PORT, g_metrics, g_logger, &udp) == DISTRIC_OK);
    health_update_status(udp_health, HEALTH_UP, "Socket bound");
    printf("    ✓ UDP socket created\n");
    
    /* Test 1: Direct client connections (no pool) */
    printf("\n[4/8] Testing direct TCP connections (no pool)...\n");
    
    pthread_t tcp_threads[NUM_TCP_CLIENTS];
    for (int i = 0; i < NUM_TCP_CLIENTS; i++) {
        pthread_create(&tcp_threads[i], NULL, tcp_client_worker, NULL);
    }
    
    for (int i = 0; i < NUM_TCP_CLIENTS; i++) {
        pthread_join(tcp_threads[i], NULL);
    }
    
    int echoed = atomic_load(&tcp_messages_echoed);
    printf("    ✓ Direct connections: %d messages echoed\n", echoed);
    
    /* Test 2: Connection pool */
    printf("\n[5/8] Testing TCP connection pool...\n");
    
    tcp_pool_t* pool;
    assert(tcp_pool_create(10, g_metrics, g_logger, &pool) == DISTRIC_OK);
    
    pthread_t pool_threads[5];
    for (int i = 0; i < 5; i++) {
        pthread_create(&pool_threads[i], NULL, pool_test_worker, pool);
    }
    
    for (int i = 0; i < 5; i++) {
        pthread_join(pool_threads[i], NULL);
    }
    
    size_t pool_size;
    uint64_t pool_hits, pool_misses;
    tcp_pool_get_stats(pool, &pool_size, &pool_hits, &pool_misses);
    
    int pool_success = atomic_load(&pool_test_success);
    printf("    ✓ Pool test: %d successful, hits=%lu, misses=%lu, size=%zu\n",
           pool_success, pool_hits, pool_misses, pool_size);
    
    /* Test 3: UDP */
    printf("\n[6/8] Testing UDP...\n");
    
    pthread_t udp_sender_thread, udp_receiver_thread;
    pthread_create(&udp_receiver_thread, NULL, udp_receiver_worker, udp);
    sleep(1);
    pthread_create(&udp_sender_thread, NULL, udp_sender_worker, udp);
    
    pthread_join(udp_sender_thread, NULL);
    sleep(2);
    atomic_store(&udp_receiver_running, false);
    pthread_join(udp_receiver_thread, NULL);
    
    int udp_received = atomic_load(&udp_packets_received);
    printf("    ✓ UDP: %d/%d packets received\n", udp_received, NUM_UDP_PACKETS);
    
    /* Export metrics */
    printf("\n[7/8] Exporting metrics...\n");
    char* metrics_output;
    size_t metrics_size;
    metrics_export_prometheus(g_metrics, &metrics_output, &metrics_size);
    printf("%s\n", metrics_output);
    free(metrics_output);
    
    /* Cleanup */
    printf("\n[8/8] Shutting down...\n");
    
    tcp_pool_destroy(pool);
    tcp_server_destroy(server);
    udp_close(udp);
    
    health_destroy(g_health);
    log_destroy(g_logger);
    metrics_destroy(g_metrics);
    
    printf("    ✓ Cleanup complete\n");
    
    /* Final verdict */
    printf("\n=== Test Result ===\n");
    
    bool tcp_ok = (echoed >= NUM_TCP_CLIENTS * 5); /* 50% threshold */
    bool pool_ok = (pool_success >= 10); /* At least 10 successful pool operations */
    bool udp_ok = (udp_received >= NUM_UDP_PACKETS * 0.85);
    
    if (tcp_ok && pool_ok && udp_ok) {
        printf("✓ PASS: All tests successful!\n");
        printf("  - TCP direct: %d messages echoed\n", echoed);
        printf("  - TCP pool: %d successful operations\n", pool_success);
        printf("  - UDP: %d packets received\n", udp_received);
        return 0;
    } else {
        printf("✗ FAIL\n");
        if (!tcp_ok) printf("  - TCP direct failed\n");
        if (!pool_ok) printf("  - TCP pool failed\n");
        if (!udp_ok) printf("  - UDP failed\n");
        return 1;
    }
}