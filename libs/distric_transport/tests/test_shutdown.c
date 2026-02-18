/**
 * @file test_shutdown.c
 * @brief Unit tests — safe shutdown and resource cleanup.
 *
 * Covers:
 *  1. tcp_server_destroy() while clients are connected does not crash.
 *  2. tcp_pool_destroy() closes all idle connections cleanly.
 *  3. tcp_pool_destroy() while connections are in use: waits then cleans up.
 *  4. udp_close() reports correct drop totals in the log.
 *  5. tcp_close(NULL) is a no-op (no crash).
 *  6. Multiple tcp_server_stop() calls are idempotent.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <distric_transport.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define SD_PORT_BASE  19600

static int tests_passed = 0;
static int tests_failed = 0;

static metrics_registry_t* g_metrics = NULL;
static logger_t*           g_logger  = NULL;

#define ASSERT_TRUE(expr) do {                                            \
    if (!(expr)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
        tests_failed++;                                                    \
        return;                                                            \
    }                                                                     \
} while(0)

#define ASSERT_OK(expr) do {                                              \
    distric_err_t _e = (expr);                                            \
    if (_e != DISTRIC_OK) {                                               \
        fprintf(stderr, "FAIL %s:%d: %s = %d\n",                         \
                __FILE__, __LINE__, #expr, _e);                           \
        tests_failed++;                                                    \
        return;                                                            \
    }                                                                     \
} while(0)

#define TEST_START() printf("[TEST] %s\n", __func__)
#define TEST_PASS()  do { printf("[PASS] %s\n", __func__); tests_passed++; } while(0)

/* ============================================================================
 * Test 1: null safety
 * ========================================================================= */

void test_null_safety(void) {
    TEST_START();
    tcp_close(NULL);         /* Must not crash */
    udp_close(NULL);         /* Must not crash */
    tcp_server_destroy(NULL);
    tcp_pool_destroy(NULL);
    printf("    All NULL calls survived\n");
    TEST_PASS();
}

/* ============================================================================
 * Test 2: server destroy after clients connect and disconnect
 * ========================================================================= */

static _Atomic int g_echo_count = 0;

static void on_slow_echo(tcp_connection_t* conn, void* ud) {
    (void)ud;
    char buf[128];
    int n = tcp_recv(conn, buf, sizeof(buf), 2000);
    if (n > 0) {
        tcp_send(conn, buf, n);
        atomic_fetch_add(&g_echo_count, 1);
    }
    tcp_close(conn);
}

void test_server_destroy_with_clients(void) {
    TEST_START();
    atomic_store(&g_echo_count, 0);

    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", SD_PORT_BASE, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_slow_echo, NULL));
    usleep(100000);

    /* Connect a few clients and do a round-trip */
    for (int i = 0; i < 5; i++) {
        tcp_connection_t* conn;
        if (tcp_connect("127.0.0.1", SD_PORT_BASE, 3000, NULL,
                        g_metrics, g_logger, &conn) == DISTRIC_OK) {
            tcp_send(conn, "ping", 4);
            char buf[16];
            tcp_recv(conn, buf, sizeof(buf), 2000);
            tcp_close(conn);
        }
    }

    usleep(200000);  /* Let handlers finish */

    /* Destroy while server is running — must not crash */
    tcp_server_destroy(server);
    printf("    Server destroyed cleanly (echoed=%d)\n",
           atomic_load(&g_echo_count));
    ASSERT_TRUE(atomic_load(&g_echo_count) > 0);
    TEST_PASS();
}

/* ============================================================================
 * Test 3: double stop is idempotent
 * ========================================================================= */

void test_server_stop_idempotent(void) {
    TEST_START();

    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", SD_PORT_BASE + 1, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_slow_echo, NULL));
    usleep(50000);

    ASSERT_OK(tcp_server_stop(server));
    /* Second stop on an already-stopped server must return OK */
    ASSERT_OK(tcp_server_stop(server));
    printf("    Double stop survived\n");

    tcp_server_destroy(server);
    TEST_PASS();
}

/* ============================================================================
 * Test 4: pool destroy with idle connections
 * ========================================================================= */

static void on_noop(tcp_connection_t* conn, void* ud) {
    (void)ud;
    usleep(50000);  /* Hold connection briefly */
    tcp_close(conn);
}

void test_pool_destroy_idle(void) {
    TEST_START();

    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", SD_PORT_BASE + 2, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_noop, NULL));
    usleep(100000);

    tcp_pool_t* pool;
    ASSERT_OK(tcp_pool_create(10, g_metrics, g_logger, &pool));

    /* Acquire, use, and release some connections */
    for (int i = 0; i < 3; i++) {
        tcp_connection_t* conn;
        if (tcp_pool_acquire(pool, "127.0.0.1", SD_PORT_BASE + 2, &conn) == DISTRIC_OK) {
            tcp_send(conn, "hi", 2);
            tcp_pool_release(pool, conn);
        }
    }

    /* Destroy with idle connections in pool */
    tcp_pool_destroy(pool);
    printf("    Pool destroyed with idle connections\n");

    tcp_server_destroy(server);
    TEST_PASS();
}

/* ============================================================================
 * Test 5: pool in-use connections not lost during destroy
 * ========================================================================= */

typedef struct {
    tcp_pool_t*       pool;
    tcp_connection_t* conn;
    _Atomic bool      release_done;
} release_args_t;

static void* delayed_release(void* arg) {
    release_args_t* a = (release_args_t*)arg;
    usleep(150000);  /* 150ms: destroy will be called 100ms into this */
    tcp_pool_release(a->pool, a->conn);
    atomic_store(&a->release_done, true);
    return NULL;
}

void test_pool_destroy_in_use(void) {
    TEST_START();

    tcp_server_t* server;
    ASSERT_OK(tcp_server_create("127.0.0.1", SD_PORT_BASE + 3, g_metrics, g_logger, &server));
    ASSERT_OK(tcp_server_start(server, on_noop, NULL));
    usleep(100000);

    tcp_pool_t* pool;
    ASSERT_OK(tcp_pool_create(5, g_metrics, g_logger, &pool));

    tcp_connection_t* conn;
    ASSERT_OK(tcp_pool_acquire(pool, "127.0.0.1", SD_PORT_BASE + 3, &conn));

    /* Start delayed release in background */
    release_args_t args;
    args.pool = pool;
    args.conn = conn;
    atomic_init(&args.release_done, false);

    pthread_t tid;
    pthread_create(&tid, NULL, delayed_release, &args);

    usleep(100000);  /* 100ms: pool destroy called while conn is still in use */
    tcp_pool_destroy(pool);  /* Should wait up to 500ms for in-use connections */

    pthread_join(tid, NULL);
    printf("    Pool destroy with in-use connection: survived\n");

    tcp_server_destroy(server);
    TEST_PASS();
}

/* ============================================================================
 * Test 6: UDP close reports drop count
 * ========================================================================= */

void test_udp_close_reports_drops(void) {
    TEST_START();

    udp_rate_limit_config_t rl = { .rate_limit_pps = 5, .burst_size = 5 };
    udp_socket_t* sender;
    udp_socket_t* receiver;

    ASSERT_OK(udp_socket_create("127.0.0.1", 0,               NULL,
                                g_metrics, g_logger, &sender));
    ASSERT_OK(udp_socket_create("127.0.0.1", SD_PORT_BASE + 4, &rl,
                                g_metrics, g_logger, &receiver));

    /* Flood to generate drops */
    for (int i = 0; i < 100; i++) {
        udp_send(sender, "drop", 4, "127.0.0.1", SD_PORT_BASE + 4);
    }
    usleep(20000);

    /* Drain */
    char buf[16];
    while (udp_recv(receiver, buf, sizeof(buf), NULL, NULL, 5) > 0) {}

    uint64_t drops = udp_get_drop_count(receiver);
    printf("    UDP drops before close: %llu\n", (unsigned long long)drops);
    ASSERT_TRUE(drops > 0);

    /* Close must log drop summary and not crash */
    udp_close(sender);
    udp_close(receiver);
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Transport — Shutdown & Resource Cleanup Tests ===\n\n");

    metrics_init(&g_metrics);
    log_init(&g_logger, STDOUT_FILENO, LOG_MODE_SYNC);

    test_null_safety();
    test_server_destroy_with_clients();
    test_server_stop_idempotent();
    test_pool_destroy_idle();
    test_pool_destroy_in_use();
    test_udp_close_reports_drops();

    log_destroy(g_logger);
    metrics_destroy(g_metrics);

    printf("\n=== Results: Passed=%d  Failed=%d ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}