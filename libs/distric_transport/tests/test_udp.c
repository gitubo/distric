/**
 * @file test_udp.c
 * @brief Unit tests for UDP socket
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <distric_transport.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

#define TEST_PORT 19300
#define TEST_MESSAGE "Hello, UDP!"

static int tests_passed = 0;
static int tests_failed = 0;

static metrics_registry_t* g_metrics = NULL;
static logger_t* g_logger = NULL;

#define ASSERT_OK(expr) do { \
    distric_err_t _err = (expr); \
    if (_err != DISTRIC_OK) { \
        fprintf(stderr, "FAIL: %s returned %d\n", #expr, _err); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_START() printf("\n[TEST] %s...\n", __func__)
#define TEST_PASS() do { \
    printf("[PASS] %s\n", __func__); \
    tests_passed++; \
} while(0)

void test_udp_socket_create(void) {
    TEST_START();
    
    udp_socket_t* udp;
    ASSERT_OK(udp_socket_create("127.0.0.1", TEST_PORT, g_metrics, g_logger, &udp));
    assert(udp != NULL);
    
    udp_close(udp);
    TEST_PASS();
}

void test_udp_send_receive(void) {
    TEST_START();
    
    /* Create sender and receiver */
    udp_socket_t* sender;
    udp_socket_t* receiver;
    
    ASSERT_OK(udp_socket_create("127.0.0.1", 0, g_metrics, g_logger, &sender));
    ASSERT_OK(udp_socket_create("127.0.0.1", TEST_PORT + 1, g_metrics, g_logger, &receiver));
    
    /* Send datagram */
    int sent = udp_send(sender, TEST_MESSAGE, strlen(TEST_MESSAGE), 
                       "127.0.0.1", TEST_PORT + 1);
    assert(sent == (int)strlen(TEST_MESSAGE));
    
    /* Receive datagram */
    char buffer[256];
    char src_addr[256];
    uint16_t src_port;
    
    int received = udp_recv(receiver, buffer, sizeof(buffer), 
                           src_addr, &src_port, 5000);
    
    assert(received == (int)strlen(TEST_MESSAGE));
    buffer[received] = '\0';
    assert(strcmp(buffer, TEST_MESSAGE) == 0);
    
    printf("    Received: '%s' from %s:%u\n", buffer, src_addr, src_port);
    
    udp_close(sender);
    udp_close(receiver);
    TEST_PASS();
}

void test_udp_multiple_packets(void) {
    TEST_START();
    
    udp_socket_t* sender;
    udp_socket_t* receiver;
    
    ASSERT_OK(udp_socket_create("127.0.0.1", 0, g_metrics, g_logger, &sender));
    ASSERT_OK(udp_socket_create("127.0.0.1", TEST_PORT + 2, g_metrics, g_logger, &receiver));
    
    /* Send multiple packets */
    #define NUM_PACKETS 100
    for (int i = 0; i < NUM_PACKETS; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Packet %d", i);
        udp_send(sender, msg, strlen(msg), "127.0.0.1", TEST_PORT + 2);
        usleep(100); /* Small delay */
    }
    
    /* Receive all packets */
    int received_count = 0;
    for (int i = 0; i < NUM_PACKETS; i++) {
        char buffer[256];
        char src_addr[256];
        uint16_t src_port;
        
        int received = udp_recv(receiver, buffer, sizeof(buffer), 
                               src_addr, &src_port, 1000);
        if (received > 0) {
            received_count++;
        }
    }
    
    printf("    Sent: %d, Received: %d packets\n", NUM_PACKETS, received_count);
    
    /* UDP may lose packets, but we should receive most */
    assert(received_count >= NUM_PACKETS * 0.9); /* 90% threshold */
    
    udp_close(sender);
    udp_close(receiver);
    TEST_PASS();
}

void test_udp_timeout(void) {
    TEST_START();
    
    udp_socket_t* udp;
    ASSERT_OK(udp_socket_create("127.0.0.1", TEST_PORT + 3, g_metrics, g_logger, &udp));
    
    /* Try to receive with timeout (should timeout) */
    char buffer[256];
    char src_addr[256];
    uint16_t src_port;
    
    int received = udp_recv(udp, buffer, sizeof(buffer), 
                           src_addr, &src_port, 500);
    
    assert(received == 0); /* Timeout */
    printf("    Correctly timed out after 500ms\n");
    
    udp_close(udp);
    TEST_PASS();
}

int main(void) {
    printf("=== DistriC Transport - UDP Tests ===\n");
    
    metrics_init(&g_metrics);
    log_init(&g_logger, STDOUT_FILENO, LOG_MODE_SYNC);
    
    test_udp_socket_create();
    test_udp_send_receive();
    test_udp_multiple_packets();
    test_udp_timeout();
    
    log_destroy(g_logger);
    metrics_destroy(g_metrics);
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}