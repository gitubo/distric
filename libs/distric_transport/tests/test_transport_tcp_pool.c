/**
 * @file test_tcp_pool.c
 * @brief Unit tests for TCP connection pool - FIXED VERSION
 */

#include <distric_transport.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define TEST_PORT 19200

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

static void on_echo(tcp_connection_t* conn, void* userdata) {
    (void)userdata;
    char buffer[256];
    int received = tcp_recv(conn, buffer, sizeof(buffer), 5000);
    if (received > 0) {
        tcp_send(conn, buffer, received);
    }
    tcp_close(conn);
}

void test_pool_create(void) {
    TEST_START();
    
    tcp_pool_t* pool;
    ASSERT_OK(tcp_pool_create(10, g_metrics, g_logger, &pool));
    assert(pool != NULL);
    
    tcp_pool_destroy(pool);
    TEST_PASS();
}

void test_pool_connection_reuse(void) {
    TEST_START();
    
    /* Start server */
    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", TEST_PORT, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_echo, NULL));
    sleep(1);
    
    /* Create pool */
    tcp_pool_t* pool;
    ASSERT_OK(tcp_pool_create(5, g_metrics, g_logger, &pool));
    
    /* First acquire (miss) */
    tcp_connection_t* conn1;
    ASSERT_OK(tcp_pool_acquire(pool, "127.0.0.1", TEST_PORT, &conn1));
    tcp_pool_release(pool, conn1);
    
    /* Second acquire (should be hit - reuse) */
    tcp_connection_t* conn2;
    ASSERT_OK(tcp_pool_acquire(pool, "127.0.0.1", TEST_PORT, &conn2));
    
    size_t size;
    uint64_t hits, misses;
    tcp_pool_get_stats(pool, &size, &hits, &misses);
    
    printf("    Pool size: %zu, Hits: %lu, Misses: %lu\n", size, hits, misses);
    assert(hits == 1);
    assert(misses == 1);
    
    tcp_pool_release(pool, conn2);
    tcp_pool_destroy(pool);
    tcp_server_destroy(server);
    TEST_PASS();
}

void test_pool_max_size(void) {
    TEST_START();
    
    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", TEST_PORT + 1, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_echo, NULL));
    sleep(1);
    
    tcp_pool_t* pool;
    ASSERT_OK(tcp_pool_create(3, g_metrics, g_logger, &pool));
    
    /* Acquire more connections than max */
    tcp_connection_t* conns[5];
    for (int i = 0; i < 5; i++) {
        ASSERT_OK(tcp_pool_acquire(pool, "127.0.0.1", TEST_PORT + 1, &conns[i]));
    }
    
    /* Release all - pool should evict excess connections */
    for (int i = 0; i < 5; i++) {
        tcp_pool_release(pool, conns[i]);
    }
    
    /* Give pool time to clean up */
    usleep(100000);
    
    size_t size;
    tcp_pool_get_stats(pool, &size, NULL, NULL);
    printf("    Final pool size: %zu (max: 3)\n", size);
    
    /* FIXED: Pool should enforce max size on release */
    assert(size <= 3);
    
    tcp_pool_destroy(pool);
    tcp_server_destroy(server);
    TEST_PASS();
}

int main(void) {
    printf("=== DistriC Transport - TCP Pool Tests ===\n");
    
    metrics_init(&g_metrics);
    log_init(&g_logger, STDOUT_FILENO, LOG_MODE_SYNC);
    
    test_pool_create();
    test_pool_connection_reuse();
    test_pool_max_size();
    
    log_destroy(g_logger);
    metrics_destroy(g_metrics);
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}