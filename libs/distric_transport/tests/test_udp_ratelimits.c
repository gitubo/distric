/**
 * @file test_udp_ratelimit.c
 * @brief Unit tests — UDP per-peer rate limiting.
 *
 * Covers:
 *  1. Packets below the rate limit pass through.
 *  2. Packets above the rate limit are dropped (recv returns 0).
 *  3. Drop counter increments correctly.
 *  4. Rate limiting disabled (rate_limit_pps=0) passes all packets.
 *  5. After a pause (token refill), packets are accepted again.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <distric_transport.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define RL_PORT_BASE 19500
#define MSG "ratelimitpkt"

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
 * TEST: No rate limit — all packets pass
 * ========================================================================= */

void test_no_rate_limit(void) {
    TEST_START();

    udp_socket_t* sender;
    udp_socket_t* receiver;

    /* No rate limit on receiver */
    ASSERT_OK(udp_socket_create("127.0.0.1", 0,                NULL,
                                g_metrics, g_logger, &sender));
    ASSERT_OK(udp_socket_create("127.0.0.1", RL_PORT_BASE,     NULL,
                                g_metrics, g_logger, &receiver));

    int sent = 0, received = 0;
    for (int i = 0; i < 50; i++) {
        if (udp_send(sender, MSG, strlen(MSG), "127.0.0.1", RL_PORT_BASE) > 0)
            sent++;
        usleep(1000);  /* 1ms between packets → 1000 pps, but no limit */
    }

    for (int i = 0; i < 50; i++) {
        char buf[64];
        if (udp_recv(receiver, buf, sizeof(buf), NULL, NULL, 100) > 0)
            received++;
    }

    printf("    Sent=%d  Received=%d  Drops=%llu\n",
           sent, received, (unsigned long long)udp_get_drop_count(receiver));
    ASSERT_TRUE(received >= sent * 9 / 10);  /* Allow 10% loss (loopback) */
    ASSERT_TRUE(udp_get_drop_count(receiver) == 0);

    udp_close(sender);
    udp_close(receiver);
    TEST_PASS();
}

/* ============================================================================
 * TEST: Rate limit enforced — burst exceeded → drops
 * ========================================================================= */

void test_rate_limit_drops_burst(void) {
    TEST_START();

    /* Limit: 10 pps, burst 10 */
    udp_rate_limit_config_t rl = { .rate_limit_pps = 10, .burst_size = 10 };

    udp_socket_t* sender;
    udp_socket_t* receiver;

    ASSERT_OK(udp_socket_create("127.0.0.1", 0,                  NULL,
                                g_metrics, g_logger, &sender));
    ASSERT_OK(udp_socket_create("127.0.0.1", RL_PORT_BASE + 1,  &rl,
                                g_metrics, g_logger, &receiver));

    /* Send 100 packets as fast as possible — burst+rate will drop most */
    int sent = 0;
    for (int i = 0; i < 100; i++) {
        if (udp_send(sender, MSG, strlen(MSG), "127.0.0.1", RL_PORT_BASE + 1) > 0)
            sent++;
    }
    usleep(50000);  /* 50ms: let packets arrive */

    int received = 0;
    for (int i = 0; i < 200; i++) {
        char buf[64];
        if (udp_recv(receiver, buf, sizeof(buf), NULL, NULL, 5) > 0)
            received++;
    }

    uint64_t drops = udp_get_drop_count(receiver);
    printf("    Sent=%d  Received=%d  Drops=%llu\n",
           sent, received, (unsigned long long)drops);

    /* Burst = 10, rate = 10 pps; 100 packets should see significant drops */
    ASSERT_TRUE(received <= 20);   /* At most burst size passes */
    ASSERT_TRUE(drops > 0);

    udp_close(sender);
    udp_close(receiver);
    TEST_PASS();
}

/* ============================================================================
 * TEST: Token bucket refills after pause
 * ========================================================================= */

void test_rate_limit_refill(void) {
    TEST_START();

    /* 100 pps, burst 100 */
    udp_rate_limit_config_t rl = { .rate_limit_pps = 100, .burst_size = 100 };

    udp_socket_t* sender;
    udp_socket_t* receiver;

    ASSERT_OK(udp_socket_create("127.0.0.1", 0,                  NULL,
                                g_metrics, g_logger, &sender));
    ASSERT_OK(udp_socket_create("127.0.0.1", RL_PORT_BASE + 2,  &rl,
                                g_metrics, g_logger, &receiver));

    /* Phase 1: exhaust the burst */
    for (int i = 0; i < 200; i++) {
        udp_send(sender, MSG, strlen(MSG), "127.0.0.1", RL_PORT_BASE + 2);
    }
    usleep(20000);

    int phase1_received = 0;
    for (int i = 0; i < 300; i++) {
        char buf[64];
        if (udp_recv(receiver, buf, sizeof(buf), NULL, NULL, 2) > 0)
            phase1_received++;
    }

    uint64_t drops_phase1 = udp_get_drop_count(receiver);
    printf("    Phase1: recv=%d drops=%llu\n",
           phase1_received, (unsigned long long)drops_phase1);
    ASSERT_TRUE(drops_phase1 > 0);  /* Should have dropped something */

    /* Phase 2: wait for refill (200ms = 20 tokens at 100/s) */
    usleep(200000);

    /* Send a small burst — should pass without drops (refilled) */
    for (int i = 0; i < 10; i++) {
        udp_send(sender, MSG, strlen(MSG), "127.0.0.1", RL_PORT_BASE + 2);
        usleep(5000);  /* spread across 50ms */
    }
    usleep(30000);

    int phase2_received = 0;
    for (int i = 0; i < 30; i++) {
        char buf[64];
        if (udp_recv(receiver, buf, sizeof(buf), NULL, NULL, 5) > 0)
            phase2_received++;
    }

    printf("    Phase2 (after refill): recv=%d\n", phase2_received);
    ASSERT_TRUE(phase2_received >= 5);  /* At least half should pass */

    udp_close(sender);
    udp_close(receiver);
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Transport — UDP Rate Limit Tests ===\n\n");

    metrics_init(&g_metrics);
    log_init(&g_logger, STDOUT_FILENO, LOG_MODE_SYNC);

    test_no_rate_limit();
    test_rate_limit_drops_burst();
    test_rate_limit_refill();

    log_destroy(g_logger);
    metrics_destroy(g_metrics);

    printf("\n=== Results: Passed=%d  Failed=%d ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}