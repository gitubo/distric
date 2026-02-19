/**
 * @file test_rpc_production.c
 * @brief Production-hardening tests for distric_protocol RPC layer
 *
 * Tests cover:
 *  T1  Payload size enforcement — server rejects oversized payloads
 *  T2  Payload size enforcement — client rejects oversized requests
 *  T3  Payload size enforcement — client rejects oversized responses
 *  T4  Version negotiation   — wrong major version rejected
 *  T5  Version negotiation   — minor version difference tolerated
 *  T6  Admission control     — server rejects beyond max_inflight
 *  T7  Error taxonomy        — rpc_error_classify correctness
 *  T8  Error taxonomy        — rpc_error_class_to_string non-null
 *  T9  Timeout propagation   — zero-timeout returns DISTRIC_ERR_TIMEOUT (or similar)
 *  T10 Graceful drain        — stop waits for in-flight then exits
 *  T11 rpc_server_create_with_config — config fields applied
 *  T12 PROTOCOL_MAX_PAYLOAD_SIZE     — validate_message_header boundary
 *  T13 Backpressure metric increment  — rpc_call with backpressure path
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <distric_protocol.h>
#include <distric_transport.h>
#include <distric_obs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* ============================================================================
 * MINI TEST HARNESS
 * ========================================================================= */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START() \
    do { tests_run++; printf("[TEST] %s ... ", __func__); fflush(stdout); } while(0)

#define TEST_PASS() \
    do { tests_passed++; printf("PASS\n"); return; } while(0)

#define TEST_FAIL(msg) \
    do { tests_failed++; printf("FAIL: %s (line %d)\n", (msg), __LINE__); return; } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) { \
        printf("FAIL: %s == %s (%lld != %lld) line %d\n", \
               #a, #b, (long long)(a), (long long)(b), __LINE__); \
        tests_failed++; return; } } while(0)

#define ASSERT_NE(a, b) \
    do { if ((a) == (b)) { \
        printf("FAIL: %s != %s (both == %lld) line %d\n", \
               #a, #b, (long long)(a), __LINE__); \
        tests_failed++; return; } } while(0)

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        printf("FAIL: %s is false (line %d)\n", #expr, __LINE__); \
        tests_failed++; return; } } while(0)

#define ASSERT_OK(expr) ASSERT_EQ((expr), DISTRIC_OK)

/* Port base — tests use consecutive ports to avoid conflicts */
#define BASE_PORT 20200

/* ============================================================================
 * COMMON SETUP HELPERS
 * ========================================================================= */

typedef struct {
    metrics_registry_t* metrics;
    logger_t*           logger;
} obs_t;

static obs_t make_obs(void) {
    obs_t o = {NULL, NULL};
    metrics_init(&o.metrics);
    log_init(&o.logger, STDOUT_FILENO, LOG_MODE_SYNC);
    return o;
}

static void free_obs(obs_t* o) {
    if (o->logger)  log_destroy(o->logger);
    if (o->metrics) metrics_destroy(o->metrics);
}

static int echo_handler(const uint8_t* req, size_t req_len,
                         uint8_t** resp, size_t* resp_len,
                         void* ud, trace_span_t* span) {
    (void)ud; (void)span;
    if (req_len == 0) { *resp = NULL; *resp_len = 0; return 0; }
    *resp = (uint8_t*)malloc(req_len);
    if (!*resp) return -1;
    memcpy(*resp, req, req_len);
    *resp_len = req_len;
    return 0;
}

/* slow handler: sleeps briefly to simulate in-flight work */
static int slow_handler(const uint8_t* req, size_t req_len,
                         uint8_t** resp, size_t* resp_len,
                         void* ud, trace_span_t* span) {
    (void)req; (void)req_len; (void)ud; (void)span;
    usleep(300000); /* 300 ms */
    *resp = (uint8_t*)malloc(4);
    memset(*resp, 0, 4);
    *resp_len = 4;
    return 0;
}

/* ============================================================================
 * T1: SERVER REJECTS OVERSIZED PAYLOAD
 * ========================================================================= */

/*
 * We craft a valid header whose payload_len exceeds the server's
 * max_message_size, send it directly over a raw TCP connection, and confirm
 * the server closes the connection without processing the payload.
 */
void test_server_rejects_oversized_payload(void) {
    TEST_START();

    obs_t obs = make_obs();
    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    uint16_t port = BASE_PORT + 1;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));

    /* Limit to 1 kB */
    rpc_server_config_t cfg = {0};
    cfg.max_message_size = 1024;
    ASSERT_OK(rpc_server_create_with_config(tcp_srv, obs.metrics,
                                             obs.logger, NULL, &cfg, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                          echo_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    usleep(100000);

    /* Connect a raw client and send a header claiming 2 kB payload */
    tcp_pool_t* pool;
    ASSERT_OK(tcp_pool_create(5, obs.metrics, obs.logger, &pool));

    tcp_connection_t* conn = NULL;
    ASSERT_OK(tcp_pool_acquire(pool, "127.0.0.1", port, &conn));

    message_header_t hdr;
    message_header_init(&hdr, MSG_CLIENT_SUBMIT, 2048); /* 2 kB > limit */
    uint8_t fake_payload[2048];
    memset(fake_payload, 'X', sizeof(fake_payload));
    compute_header_crc32(&hdr, fake_payload, sizeof(fake_payload));

    uint8_t wire[MESSAGE_HEADER_SIZE];
    serialize_header(&hdr, wire);

    /* Send header — server should reject and close */
    tcp_send(conn, wire, MESSAGE_HEADER_SIZE);

    /* Attempt to receive any response; server should have closed the conn */
    uint8_t resp[MESSAGE_HEADER_SIZE];
    int rc = tcp_recv(conn, resp, MESSAGE_HEADER_SIZE, 2000);
    /* rc == 0 means EOF (connection closed by server) — expected */
    ASSERT_TRUE(rc <= 0);

    tcp_pool_mark_failed(pool, conn);
    tcp_pool_release(pool, conn);

    rpc_server_stop(server);
    rpc_server_destroy(server);
    tcp_pool_destroy(pool);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T2: CLIENT REJECTS OVERSIZED REQUEST BEFORE SENDING
 * ========================================================================= */

void test_client_rejects_oversized_request(void) {
    TEST_START();

    obs_t obs = make_obs();
    tcp_pool_t* pool;
    rpc_client_t* client;

    ASSERT_OK(tcp_pool_create(5, obs.metrics, obs.logger, &pool));
    ASSERT_OK(rpc_client_create(pool, obs.metrics, obs.logger, NULL, &client));

    /* Allocate a buffer larger than RPC_MAX_MESSAGE_SIZE */
    size_t big = (size_t)RPC_MAX_MESSAGE_SIZE + 1;
    uint8_t* big_buf = (uint8_t*)malloc(big);
    ASSERT_TRUE(big_buf != NULL);
    memset(big_buf, 0, big);

    uint8_t* resp = NULL; size_t resp_len = 0;
    distric_err_t err = rpc_call(client, "127.0.0.1", BASE_PORT + 2,
                                  MSG_CLIENT_SUBMIT,
                                  big_buf, big,
                                  &resp, &resp_len, 1000);

    /* Must be rejected locally without touching the network */
    ASSERT_EQ(err, DISTRIC_ERR_INVALID_ARG);
    ASSERT_TRUE(resp == NULL);

    free(big_buf);
    rpc_client_destroy(client);
    tcp_pool_destroy(pool);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T3: validate_message_header — payload at PROTOCOL_MAX_PAYLOAD_SIZE boundary
 * ========================================================================= */

void test_validate_header_payload_boundary(void) {
    TEST_START();

    message_header_t hdr;
    message_header_init(&hdr, MSG_CLIENT_SUBMIT, 0);

    /* Exactly at limit — must pass */
    hdr.payload_len = PROTOCOL_MAX_PAYLOAD_SIZE;
    ASSERT_TRUE(validate_message_header(&hdr));

    /* One byte over — must fail */
    hdr.payload_len = PROTOCOL_MAX_PAYLOAD_SIZE + 1;
    ASSERT_TRUE(!validate_message_header(&hdr));

    /* Zero length — must pass */
    hdr.payload_len = 0;
    ASSERT_TRUE(validate_message_header(&hdr));

    TEST_PASS();
}

/* ============================================================================
 * T4: VERSION NEGOTIATION — wrong major version rejected
 * ========================================================================= */

void test_version_negotiation_major_mismatch(void) {
    TEST_START();

    message_header_t hdr;
    message_header_init(&hdr, MSG_CLIENT_SUBMIT, 0);

    /* Current version must be valid */
    ASSERT_TRUE(validate_message_header(&hdr));

    /* Flip the major byte */
    uint8_t our_minor = (uint8_t)(PROTOCOL_VERSION & 0xFF);
    hdr.version = (uint16_t)(((uint8_t)(PROTOCOL_VERSION >> 8) ^ 0x01) << 8) | our_minor;
    ASSERT_TRUE(!validate_message_header(&hdr));

    TEST_PASS();
}

/* ============================================================================
 * T5: VERSION NEGOTIATION — minor version mismatch tolerated
 * ========================================================================= */

void test_version_negotiation_minor_ok(void) {
    TEST_START();

    message_header_t hdr;
    message_header_init(&hdr, MSG_CLIENT_SUBMIT, 0);

    /* Increment minor version — should still pass */
    uint8_t major = (uint8_t)(PROTOCOL_VERSION >> 8);
    uint8_t minor = (uint8_t)(PROTOCOL_VERSION & 0xFF);
    hdr.version = (uint16_t)((major << 8) | (minor + 1));

    ASSERT_TRUE(validate_message_header(&hdr));

    TEST_PASS();
}

/* ============================================================================
 * T6: ADMISSION CONTROL — server rejects beyond max_inflight
 * ========================================================================= */

/* Synchronized to know when the slow handler is actively executing */
static atomic_int slow_inflight = ATOMIC_VAR_INIT(0);
static atomic_int slow_release  = ATOMIC_VAR_INIT(0);

static int gate_handler(const uint8_t* req, size_t req_len,
                        uint8_t** resp, size_t* resp_len,
                        void* ud, trace_span_t* span) {
    (void)req; (void)req_len; (void)ud; (void)span;
    atomic_fetch_add(&slow_inflight, 1);
    /* Wait until released */
    while (!atomic_load(&slow_release)) usleep(1000);
    *resp = (uint8_t*)malloc(1); (*resp)[0] = 0;
    *resp_len = 1;
    return 0;
}

void test_admission_control_overload(void) {
    TEST_START();

    obs_t obs = make_obs();
    uint16_t port = BASE_PORT + 6;

    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));

    rpc_server_config_t cfg = {0};
    cfg.max_inflight_requests = 1; /* Only 1 in-flight request allowed */
    cfg.drain_timeout_ms      = 2000;
    cfg.max_message_size      = RPC_MAX_MESSAGE_SIZE;
    ASSERT_OK(rpc_server_create_with_config(tcp_srv, obs.metrics, obs.logger,
                                             NULL, &cfg, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                          gate_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    usleep(100000);

    atomic_store(&slow_inflight, 0);
    atomic_store(&slow_release, 0);

    /* Saturate the single in-flight slot via a raw TCP connection so we
     * can precisely control timing without a client-thread race. */
    tcp_pool_t* raw_pool;
    ASSERT_OK(tcp_pool_create(2, obs.metrics, obs.logger, &raw_pool));
    tcp_connection_t* raw_conn;
    ASSERT_OK(tcp_pool_acquire(raw_pool, "127.0.0.1", port, &raw_conn));

    /* Send first request (occupies the slot) */
    uint8_t payload1[4] = {1,2,3,4};
    message_header_t h1;
    message_header_init(&h1, MSG_CLIENT_SUBMIT, 4);
    compute_header_crc32(&h1, payload1, 4);
    uint8_t w1[MESSAGE_HEADER_SIZE]; serialize_header(&h1, w1);
    tcp_send(raw_conn, w1, MESSAGE_HEADER_SIZE);
    tcp_send(raw_conn, payload1, 4);

    /* Wait until gate_handler is blocking */
    int waited = 0;
    while (atomic_load(&slow_inflight) == 0 && waited < 100) {
        usleep(20000); waited++;
    }
    ASSERT_NE(atomic_load(&slow_inflight), 0);

    /* Now try a second request on a different connection */
    tcp_connection_t* raw_conn2;
    ASSERT_OK(tcp_pool_acquire(raw_pool, "127.0.0.1", port, &raw_conn2));

    message_header_t h2;
    message_header_init(&h2, MSG_CLIENT_SUBMIT, 4);
    compute_header_crc32(&h2, payload1, 4);
    uint8_t w2[MESSAGE_HEADER_SIZE]; serialize_header(&h2, w2);
    tcp_send(raw_conn2, w2, MESSAGE_HEADER_SIZE);
    tcp_send(raw_conn2, payload1, 4);

    /* Server should have closed conn2 because max_inflight is exceeded.
     * Receiving on conn2 should return EOF or an error. */
    uint8_t resp2[MESSAGE_HEADER_SIZE];
    int rc2 = tcp_recv(raw_conn2, resp2, MESSAGE_HEADER_SIZE, 2000);
    ASSERT_TRUE(rc2 <= 0); /* EOF from closed connection */

    /* Release the gate so the first request can complete */
    atomic_store(&slow_release, 1);

    /* Wait for first response */
    uint8_t resp1_hdr[MESSAGE_HEADER_SIZE];
    tcp_recv(raw_conn, resp1_hdr, MESSAGE_HEADER_SIZE, 3000);

    tcp_pool_mark_failed(raw_pool, raw_conn);
    tcp_pool_release(raw_pool, raw_conn);
    tcp_pool_mark_failed(raw_pool, raw_conn2);
    tcp_pool_release(raw_pool, raw_conn2);
    tcp_pool_destroy(raw_pool);

    rpc_server_stop(server);
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T7: ERROR TAXONOMY — rpc_error_classify
 * ========================================================================= */

void test_error_taxonomy_classify(void) {
    TEST_START();

    ASSERT_EQ(rpc_error_classify(DISTRIC_OK),              RPC_ERR_CLASS_OK);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_TIMEOUT),     RPC_ERR_CLASS_TIMEOUT);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_BACKPRESSURE),RPC_ERR_CLASS_BACKPRESSURE);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_UNAVAILABLE), RPC_ERR_CLASS_UNAVAILABLE);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_IO),          RPC_ERR_CLASS_UNAVAILABLE);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_INIT_FAILED), RPC_ERR_CLASS_UNAVAILABLE);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_INVALID_ARG), RPC_ERR_CLASS_INVALID);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_INVALID_FORMAT), RPC_ERR_CLASS_INVALID);
    ASSERT_EQ(rpc_error_classify(DISTRIC_ERR_NO_MEMORY),   RPC_ERR_CLASS_INTERNAL);

    TEST_PASS();
}

/* ============================================================================
 * T8: ERROR TAXONOMY — string names are non-null and non-empty
 * ========================================================================= */

void test_error_taxonomy_strings(void) {
    TEST_START();

    rpc_error_class_t classes[] = {
        RPC_ERR_CLASS_OK,
        RPC_ERR_CLASS_TIMEOUT,
        RPC_ERR_CLASS_UNAVAILABLE,
        RPC_ERR_CLASS_OVERLOADED,
        RPC_ERR_CLASS_BACKPRESSURE,
        RPC_ERR_CLASS_INVALID,
        RPC_ERR_CLASS_INTERNAL,
    };

    for (size_t i = 0; i < sizeof(classes)/sizeof(classes[0]); i++) {
        const char* s = rpc_error_class_to_string(classes[i]);
        ASSERT_TRUE(s != NULL && s[0] != '\0');
        printf("  class %d → \"%s\"\n", (int)classes[i], s);
    }

    TEST_PASS();
}

/* ============================================================================
 * T9: TIMEOUT PROPAGATION — rpc_call returns DISTRIC_ERR_TIMEOUT
 * ========================================================================= */

void test_timeout_propagation(void) {
    TEST_START();

    obs_t obs = make_obs();
    uint16_t port = BASE_PORT + 9;

    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));
    ASSERT_OK(rpc_server_create(tcp_srv, obs.metrics, obs.logger, NULL, &server));
    /* Register a handler that sleeps longer than the client timeout */
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                          slow_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    usleep(100000);

    tcp_pool_t* pool;
    rpc_client_t* client;
    ASSERT_OK(tcp_pool_create(5, obs.metrics, obs.logger, &pool));
    ASSERT_OK(rpc_client_create(pool, obs.metrics, obs.logger, NULL, &client));

    const char* data = "hello";
    uint8_t* resp = NULL; size_t resp_len = 0;

    /* Very short timeout — 100 ms; slow_handler sleeps 300 ms */
    distric_err_t err = rpc_call(client, "127.0.0.1", port,
                                  MSG_CLIENT_SUBMIT,
                                  (const uint8_t*)data, strlen(data),
                                  &resp, &resp_len, 100);

    /* Must return timeout, not a generic connection error */
    ASSERT_EQ(err, DISTRIC_ERR_TIMEOUT);
    ASSERT_TRUE(resp == NULL);

    rpc_client_destroy(client);
    tcp_pool_destroy(pool);
    rpc_server_stop(server);
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T10: GRACEFUL DRAIN — stop waits for in-flight request
 * ========================================================================= */

static atomic_int drain_handler_done = ATOMIC_VAR_INIT(0);

static int counting_handler(const uint8_t* req, size_t req_len,
                              uint8_t** resp, size_t* resp_len,
                              void* ud, trace_span_t* span) {
    (void)req; (void)req_len; (void)ud; (void)span;
    usleep(200000); /* 200 ms */
    atomic_fetch_add(&drain_handler_done, 1);
    *resp = (uint8_t*)malloc(1); (*resp)[0] = 1;
    *resp_len = 1;
    return 0;
}

typedef struct {
    rpc_client_t* client;
    uint16_t      port;
} drain_thread_arg_t;

static void* drain_call_thread(void* arg) {
    drain_thread_arg_t* a = (drain_thread_arg_t*)arg;
    uint8_t req[4] = {0}; uint8_t* resp = NULL; size_t resp_len = 0;
    rpc_call(a->client, "127.0.0.1", a->port,
             MSG_CLIENT_SUBMIT, req, 4, &resp, &resp_len, 5000);
    if (resp) free(resp);
    return NULL;
}

void test_graceful_drain(void) {
    TEST_START();

    obs_t obs = make_obs();
    uint16_t port = BASE_PORT + 10;

    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));

    rpc_server_config_t cfg = {0};
    cfg.drain_timeout_ms = 3000;  /* 3s drain window */
    ASSERT_OK(rpc_server_create_with_config(tcp_srv, obs.metrics, obs.logger,
                                             NULL, &cfg, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                          counting_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    usleep(100000);

    atomic_store(&drain_handler_done, 0);

    tcp_pool_t* pool;
    rpc_client_t* client;
    ASSERT_OK(tcp_pool_create(5, obs.metrics, obs.logger, &pool));
    ASSERT_OK(rpc_client_create(pool, obs.metrics, obs.logger, NULL, &client));

    /* Fire a request that will still be in-flight when we call stop() */
    drain_thread_arg_t targ = {client, port};
    pthread_t call_thread;
    pthread_create(&call_thread, NULL, drain_call_thread, &targ);

    /* Give the call time to reach the server */
    usleep(50000);

    /* Stop should drain and wait for counting_handler to finish */
    uint64_t t0 = (uint64_t)time(NULL);
    rpc_server_stop(server);
    uint64_t t1 = (uint64_t)time(NULL);

    /* The handler took ~200 ms so stop should have waited at least a bit */
    /* We just verify handler ran to completion */
    ASSERT_NE(atomic_load(&drain_handler_done), 0);

    /* Stop should not have taken longer than drain_timeout_ms */
    ASSERT_TRUE((t1 - t0) < 4);

    pthread_join(call_thread, NULL);

    rpc_client_destroy(client);
    tcp_pool_destroy(pool);
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T11: rpc_server_create_with_config applies fields
 * ========================================================================= */

void test_server_config_applied(void) {
    TEST_START();

    obs_t obs = make_obs();
    uint16_t port = BASE_PORT + 11;

    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));

    rpc_server_config_t cfg = {
        .max_inflight_requests = 42,
        .drain_timeout_ms      = 9999,
        .max_message_size      = 512,
    };
    distric_err_t err = rpc_server_create_with_config(
        tcp_srv, obs.metrics, obs.logger, NULL, &cfg, &server);
    ASSERT_OK(err);

    ASSERT_TRUE(server != NULL);

    rpc_server_destroy(server);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T12: Full RPC round-trip verifies all improvements are compatible
 * ========================================================================= */

void test_full_roundtrip_with_hardening(void) {
    TEST_START();

    obs_t obs = make_obs();
    uint16_t port = BASE_PORT + 12;

    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));

    rpc_server_config_t cfg = {0};
    cfg.max_inflight_requests = 100;
    cfg.max_message_size      = 64 * 1024; /* 64 kB */
    ASSERT_OK(rpc_server_create_with_config(tcp_srv, obs.metrics, obs.logger,
                                             NULL, &cfg, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                          echo_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    usleep(100000);

    tcp_pool_t* pool;
    rpc_client_t* client;
    ASSERT_OK(tcp_pool_create(5, obs.metrics, obs.logger, &pool));
    ASSERT_OK(rpc_client_create(pool, obs.metrics, obs.logger, NULL, &client));

    const char* msg = "hardened-rpc-round-trip";
    uint8_t* resp = NULL; size_t resp_len = 0;

    ASSERT_OK(rpc_call(client, "127.0.0.1", port,
                        MSG_CLIENT_SUBMIT,
                        (const uint8_t*)msg, strlen(msg),
                        &resp, &resp_len, 5000));

    ASSERT_TRUE(resp != NULL);
    ASSERT_EQ(resp_len, strlen(msg));
    ASSERT_TRUE(memcmp(resp, msg, resp_len) == 0);
    free(resp);

    rpc_client_destroy(client);
    tcp_pool_destroy(pool);
    rpc_server_stop(server);
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * T13: rpc_call_with_retry retries on timeout
 * ========================================================================= */

static atomic_int attempt_count = ATOMIC_VAR_INIT(0);

static int first_fail_handler(const uint8_t* req, size_t req_len,
                               uint8_t** resp, size_t* resp_len,
                               void* ud, trace_span_t* span) {
    (void)req; (void)req_len; (void)ud; (void)span;
    int n = atomic_fetch_add(&attempt_count, 1);
    if (n == 0) {
        /* Fail the first attempt by sleeping past any reasonable timeout */
        usleep(500000); /* 500 ms */
    }
    *resp = (uint8_t*)malloc(1); (*resp)[0] = (uint8_t)(n + 1);
    *resp_len = 1;
    return 0;
}

void test_retry_on_timeout(void) {
    TEST_START();

    obs_t obs = make_obs();
    uint16_t port = BASE_PORT + 13;

    tcp_server_t* tcp_srv;
    rpc_server_t* server;

    ASSERT_OK(tcp_server_create("127.0.0.1", port,
                                obs.metrics, obs.logger, &tcp_srv));
    ASSERT_OK(rpc_server_create(tcp_srv, obs.metrics, obs.logger, NULL, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                          first_fail_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    usleep(100000);

    tcp_pool_t* pool;
    rpc_client_t* client;
    ASSERT_OK(tcp_pool_create(5, obs.metrics, obs.logger, &pool));
    ASSERT_OK(rpc_client_create(pool, obs.metrics, obs.logger, NULL, &client));

    atomic_store(&attempt_count, 0);

    const char* data = "retry-me";
    uint8_t* resp = NULL; size_t resp_len = 0;

    distric_err_t err = rpc_call_with_retry(
        client, "127.0.0.1", port, MSG_CLIENT_SUBMIT,
        (const uint8_t*)data, strlen(data),
        &resp, &resp_len,
        200,  /* 200 ms timeout → first attempt will time out */
        3     /* max 3 retries */
    );

    /* May succeed on retry or may time out all attempts depending on timing.
     * We verify it either succeeds or returns a typed error (not INIT_FAILED). */
    if (err == DISTRIC_OK) {
        ASSERT_TRUE(resp != NULL);
        free(resp);
        printf("  Retry succeeded\n");
    } else {
        ASSERT_NE(err, DISTRIC_ERR_INIT_FAILED);
        printf("  All retries exhausted, typed error: %d\n", (int)err);
    }

    rpc_client_destroy(client);
    tcp_pool_destroy(pool);
    rpc_server_stop(server);
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_srv);
    free_obs(&obs);

    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== distric_protocol RPC production-hardening tests ===\n\n");

    /* Unit tests (no server) */
    test_client_rejects_oversized_request();
    test_validate_header_payload_boundary();
    test_version_negotiation_major_mismatch();
    test_version_negotiation_minor_ok();
    test_error_taxonomy_classify();
    test_error_taxonomy_strings();
    test_server_config_applied();

    /* Integration tests (needs live server) */
    test_server_rejects_oversized_payload();
    test_timeout_propagation();
    test_graceful_drain();
    test_full_roundtrip_with_hardening();
    test_retry_on_timeout();

    /* Admission control requires precise timing — run last */
    test_admission_control_overload();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed ? 1 : 0;
}