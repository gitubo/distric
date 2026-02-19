/**
 * @file test_tcp.c
 * @brief Unit tests for TCP server and connection
 */

#include <distric_transport.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

#define TEST_PORT 19000
#define TEST_MESSAGE "Hello, TCP!"

static int tests_passed = 0;
static int tests_failed = 0;

static metrics_registry_t* g_metrics = NULL;
static logger_t* g_logger = NULL;

/* ============================================================================
 * TEST UTILITIES
 * ========================================================================= */

#define ASSERT_OK(expr) do { \
    distric_err_t _err = (expr); \
    if (_err != DISTRIC_OK) { \
        fprintf(stderr, "FAIL: %s:%d: %s returned %d\n", \
                __FILE__, __LINE__, #expr, _err); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s is false\n", \
                __FILE__, __LINE__, #expr); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_START() printf("\n[TEST] %s...\n", __func__)
#define TEST_PASS() do { \
    printf("[PASS] %s\n", __func__); \
    tests_passed++; \
} while(0)

/* ============================================================================
 * TEST CASES
 * ========================================================================= */

static void on_echo_connection(tcp_connection_t* conn, void* userdata) {
    (void)userdata;
    
    char buffer[256];
    int received = tcp_recv(conn, buffer, sizeof(buffer) - 1, 5000);
    
    if (received > 0) {
        buffer[received] = '\0';
        tcp_send(conn, buffer, received);
    }
    
    tcp_close(conn);
}

void test_tcp_server_create(void) {
    TEST_START();
    
    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", TEST_PORT, g_metrics, g_logger, &server));
    ASSERT_TRUE(server != NULL);
    
    tcp_server_destroy(server);
    TEST_PASS();
}

void test_tcp_client_connect(void) {
    TEST_START();
    
    /* Start echo server */
    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", TEST_PORT + 1, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_echo_connection, NULL));
    
    sleep(1); /* Let server start */
    
    /* Connect client — NULL config = default send-queue settings */
    tcp_connection_t* conn;
    ASSERT_OK(tcp_connect("127.0.0.1", TEST_PORT + 1, 5000, NULL, g_metrics, g_logger, &conn));
    ASSERT_TRUE(conn != NULL);
    
    /* Send message */
    int sent = tcp_send(conn, TEST_MESSAGE, strlen(TEST_MESSAGE));
    ASSERT_TRUE(sent == (int)strlen(TEST_MESSAGE));
    
    /* Receive echo */
    char buffer[256];
    int received = tcp_recv(conn, buffer, sizeof(buffer) - 1, 5000);
    ASSERT_TRUE(received == (int)strlen(TEST_MESSAGE));
    buffer[received] = '\0';
    ASSERT_TRUE(strcmp(buffer, TEST_MESSAGE) == 0);
    
    tcp_close(conn);
    tcp_server_destroy(server);
    TEST_PASS();
}

void test_tcp_multiple_connections(void) {
    TEST_START();
    
    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", TEST_PORT + 2, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_echo_connection, NULL));
    
    sleep(1);
    
    /* Create multiple connections — NULL config = default send-queue settings */
    #define NUM_CONNS 10
    tcp_connection_t* conns[NUM_CONNS];
    
    for (int i = 0; i < NUM_CONNS; i++) {
        ASSERT_OK(tcp_connect("127.0.0.1", TEST_PORT + 2, 5000, NULL, g_metrics, g_logger, &conns[i]));
        
        char msg[64];
        snprintf(msg, sizeof(msg), "Message %d", i);
        tcp_send(conns[i], msg, strlen(msg));
    }
    
    /* Receive all responses */
    for (int i = 0; i < NUM_CONNS; i++) {
        char buffer[256];
        int received = tcp_recv(conns[i], buffer, sizeof(buffer) - 1, 5000);
        ASSERT_TRUE(received > 0);
        tcp_close(conns[i]);
    }
    
    tcp_server_destroy(server);
    TEST_PASS();
}

void test_tcp_connection_info(void) {
    TEST_START();
    
    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", TEST_PORT + 3, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_echo_connection, NULL));
    
    sleep(1);
    
    /* NULL config = default send-queue settings */
    tcp_connection_t* conn;
    ASSERT_OK(tcp_connect("127.0.0.1", TEST_PORT + 3, 5000, NULL, g_metrics, g_logger, &conn));
    
    char addr[256];
    uint16_t port;
    ASSERT_OK(tcp_get_remote_addr(conn, addr, sizeof(addr), &port));
    ASSERT_TRUE(strcmp(addr, "127.0.0.1") == 0);
    ASSERT_TRUE(port == TEST_PORT + 3);
    
    uint64_t conn_id = tcp_get_connection_id(conn);
    ASSERT_TRUE(conn_id > 0);
    
    tcp_close(conn);
    tcp_server_destroy(server);
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Transport Layer - TCP Tests ===\n");
    
    /* Initialize observability */
    metrics_init(&g_metrics);
    log_init(&g_logger, STDOUT_FILENO, LOG_MODE_SYNC);
    
    /* Run tests */
    test_tcp_server_create();
    test_tcp_client_connect();
    test_tcp_multiple_connections();
    test_tcp_connection_info();
    
    /* Cleanup */
    log_destroy(g_logger);
    metrics_destroy(g_metrics);
    
    /* Print results */
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}