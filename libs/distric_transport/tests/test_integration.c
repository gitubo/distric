/**
 * @file test_integration_shutdown_fixed.c
 * @brief Integration test with CORRECT shutdown order
 * 
 * CRITICAL FIX: Shutdown order matters!
 * 
 * WRONG ORDER (causes segfault):
 * 1. log_destroy(logger)      ← Logger freed
 * 2. udp_close(udp)           ← Tries to LOG → SEGFAULT
 * 3. tcp_server_destroy(...)  ← Tries to LOG → SEGFAULT
 * 
 * CORRECT ORDER (this file):
 * 1. Stop all producers (UDP, TCP, HTTP, tracing)
 * 2. THEN destroy logger (nothing left to log)
 * 3. Finally destroy passive data (metrics, health)
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

#define TCP_PORT 19100
#define UDP_PORT 19101
#define NUM_TCP_CLIENTS 5
#define NUM_UDP_PACKETS 50

static metrics_registry_t* g_metrics = NULL;
static logger_t* g_logger = NULL;
static health_registry_t* g_health = NULL;

static _Atomic int tcp_echoed = 0;
static _Atomic int udp_received = 0;
static _Atomic bool udp_running = true;

/* Simple echo handler */
static void on_connection(tcp_connection_t* conn, void* userdata) {
    (void)userdata;
    
    char buffer[1024];
    int received = tcp_recv(conn, buffer, sizeof(buffer), 500);
    
    if (received > 0) {
        tcp_send(conn, buffer, received);
        atomic_fetch_add(&tcp_echoed, 1);
    }
    
    tcp_close(conn);
}

/* TCP client worker */
static void* tcp_worker(void* arg) {
    (void)arg;
    
    for (int i = 0; i < 10; i++) {
        tcp_connection_t* conn;
        if (tcp_connect("127.0.0.1", TCP_PORT, 5000, g_metrics, g_logger, &conn) != DISTRIC_OK) {
            usleep(50000);
            continue;
        }
        
        char msg[64];
        snprintf(msg, sizeof(msg), "msg%d", i);
        
        tcp_send(conn, msg, strlen(msg));
        usleep(10000);
        
        char buf[1024];
        tcp_recv(conn, buf, sizeof(buf), 1000);
        
        tcp_close(conn);
        usleep(10000);
    }
    
    return NULL;
}

/* UDP workers */
static void* udp_sender(void* arg) {
    udp_socket_t* udp = (udp_socket_t*)arg;
    
    for (int i = 0; i < NUM_UDP_PACKETS; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "udp%d", i);
        udp_send(udp, msg, strlen(msg), "127.0.0.1", UDP_PORT);
        usleep(5000);
    }
    
    return NULL;
}

static void* udp_receiver(void* arg) {
    udp_socket_t* udp = (udp_socket_t*)arg;
    
    while (atomic_load(&udp_running)) {
        char buf[1024], addr[256];
        uint16_t port;
        
        int r = udp_recv(udp, buf, sizeof(buf), addr, &port, 100);
        if (r > 0) {
            atomic_fetch_add(&udp_received, 1);
        }
    }
    
    return NULL;
}

int main(void) {
    printf("=== Integration Test - Shutdown Order Fix ===\n\n");
    
    /* [1] Initialize observability */
    printf("[1/6] Init observability...\n");
    assert(metrics_init(&g_metrics) == DISTRIC_OK);
    assert(log_init(&g_logger, STDOUT_FILENO, LOG_MODE_ASYNC) == DISTRIC_OK);
    assert(health_init(&g_health) == DISTRIC_OK);
    
    health_component_t* tcp_health, *udp_health;
    health_register_component(g_health, "tcp", &tcp_health);
    health_register_component(g_health, "udp", &udp_health);
    health_update_status(tcp_health, HEALTH_UP, "Init");
    health_update_status(udp_health, HEALTH_UP, "Init");
    
    /* [2] Start TCP server */
    printf("[2/6] Start TCP server...\n");
    tcp_server_t* server;
    assert(tcp_server_create("127.0.0.1", TCP_PORT, g_metrics, g_logger, &server) == DISTRIC_OK);
    assert(tcp_server_start(server, on_connection, NULL) == DISTRIC_OK);
    sleep(1);
    
    /* [3] Create UDP socket */
    printf("[3/6] Create UDP socket...\n");
    udp_socket_t* udp;
    assert(udp_socket_create("127.0.0.1", UDP_PORT, g_metrics, g_logger, &udp) == DISTRIC_OK);
    
    /* [4] Run tests */
    printf("[4/6] Run tests...\n");
    
    pthread_t tcp_threads[NUM_TCP_CLIENTS];
    for (int i = 0; i < NUM_TCP_CLIENTS; i++) {
        pthread_create(&tcp_threads[i], NULL, tcp_worker, NULL);
    }
    
    pthread_t udp_send_thread, udp_recv_thread;
    pthread_create(&udp_recv_thread, NULL, udp_receiver, udp);
    sleep(1);
    pthread_create(&udp_send_thread, NULL, udp_sender, udp);
    
    for (int i = 0; i < NUM_TCP_CLIENTS; i++) {
        pthread_join(tcp_threads[i], NULL);
    }
    
    pthread_join(udp_send_thread, NULL);
    sleep(1);
    atomic_store(&udp_running, false);
    pthread_join(udp_recv_thread, NULL);
    
    printf("    TCP echoed: %d\n", atomic_load(&tcp_echoed));
    printf("    UDP received: %d\n", atomic_load(&udp_received));
    
    /* [5] CRITICAL: Shutdown in CORRECT order */
    printf("[5/6] Shutdown (CORRECT ORDER)...\n");
    
    /* 5a. Stop all PRODUCERS first (things that might log) */
    printf("    5a. Stop UDP (may log on close)...\n");
    udp_close(udp);
    
    printf("    5b. Stop TCP server (may log on close)...\n");
    tcp_server_destroy(server);
    
    /* 5c. Give background threads time to finish any pending logs */
    usleep(100000);  /* 100ms grace period */
    
    /* 5d. NOW safe to destroy logger (no more log producers) */
    printf("    5c. Destroy logger (now safe)...\n");
    log_destroy(g_logger);
    
    /* 5e. Finally destroy passive data structures */
    printf("    5d. Destroy metrics/health...\n");
    health_destroy(g_health);
    metrics_destroy(g_metrics);
    
    /* [6] Done */
    printf("[6/6] Complete!\n\n");
    
    int tcp_count = atomic_load(&tcp_echoed);
    int udp_count = atomic_load(&udp_received);
    
    if (tcp_count >= 20 && udp_count >= 40) {
        printf("✓ PASS (TCP=%d, UDP=%d)\n", tcp_count, udp_count);
        return 0;
    } else {
        printf("✗ FAIL (TCP=%d, UDP=%d)\n", tcp_count, udp_count);
        return 1;
    }
}