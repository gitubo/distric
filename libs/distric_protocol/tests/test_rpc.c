/**
 * @file test_rpc.c
 * @brief Comprehensive tests for RPC framework
 */

#include <distric_protocol/rpc.h>
#include <distric_protocol/messages.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START() printf("\n[TEST] %s...\n", __func__)
#define TEST_PASS() do { \
    printf("[PASS] %s\n", __func__); \
    tests_passed++; \
} while(0)

#define ASSERT_OK(expr) do { \
    distric_err_t _err = (expr); \
    if (_err != DISTRIC_OK) { \
        fprintf(stderr, "FAIL: %s returned %d\n", #expr, _err); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s is false\n", #expr); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* ============================================================================
 * ECHO HANDLER (for testing)
 * ========================================================================= */

static int echo_handler(
    const uint8_t* request,
    size_t req_len,
    uint8_t** response,
    size_t* resp_len,
    void* userdata,
    trace_span_t* span
) {
    (void)userdata;
    (void)span;
    
    /* Echo request back as response */
    *response = (uint8_t*)malloc(req_len);
    if (!*response) {
        return -1;
    }
    
    memcpy(*response, request, req_len);
    *resp_len = req_len;
    
    return 0;
}

/* ============================================================================
 * RAFT REQUEST VOTE HANDLER (realistic example)
 * ========================================================================= */

static int request_vote_handler(
    const uint8_t* request,
    size_t req_len,
    uint8_t** response,
    size_t* resp_len,
    void* userdata,
    trace_span_t* span
) {
    (void)userdata;
    (void)span;
    
    /* Deserialize request */
    raft_request_vote_t req;
    if (deserialize_raft_request_vote(request, req_len, &req) != DISTRIC_OK) {
        return -1;
    }
    
    printf("  Received RequestVote: term=%u, candidate=%s\n",
           req.term, req.candidate_id);
    
    /* Build response */
    raft_request_vote_response_t resp = {
        .term = req.term,
        .vote_granted = true
    };
    strncpy(resp.node_id, "node-1", sizeof(resp.node_id) - 1);
    
    /* Serialize response */
    return serialize_raft_request_vote_response(&resp, response, resp_len);
}

/* ============================================================================
 * TEST CASES
 * ========================================================================= */

void test_rpc_server_create() {
    TEST_START();
    
    /* Initialize dependencies */
    metrics_registry_t* metrics;
    logger_t* logger;
    tcp_server_t* tcp_server;
    
    ASSERT_OK(metrics_init(&metrics));
    ASSERT_OK(log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC));
    ASSERT_OK(tcp_server_create("127.0.0.1", 19000, metrics, logger, &tcp_server));
    
    /* Create RPC server */
    rpc_server_t* server;
    ASSERT_OK(rpc_server_create(tcp_server, metrics, logger, NULL, &server));
    ASSERT_TRUE(server != NULL);
    
    /* Cleanup */
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_server);
    log_destroy(logger);
    metrics_destroy(metrics);
    
    TEST_PASS();
}

void test_rpc_server_register_handler() {
    TEST_START();
    
    metrics_registry_t* metrics;
    logger_t* logger;
    tcp_server_t* tcp_server;
    
    ASSERT_OK(metrics_init(&metrics));
    ASSERT_OK(log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC));
    ASSERT_OK(tcp_server_create("127.0.0.1", 19001, metrics, logger, &tcp_server));
    
    rpc_server_t* server;
    ASSERT_OK(rpc_server_create(tcp_server, metrics, logger, NULL, &server));
    
    /* Register handlers */
    ASSERT_OK(rpc_server_register_handler(server, MSG_RAFT_REQUEST_VOTE,
                                         echo_handler, NULL));
    
    ASSERT_OK(rpc_server_register_handler(server, MSG_GOSSIP_PING,
                                         echo_handler, NULL));
    
    printf("  2 handlers registered successfully\n");
    
    /* Cleanup */
    rpc_server_destroy(server);
    tcp_server_destroy(tcp_server);
    log_destroy(logger);
    metrics_destroy(metrics);
    
    TEST_PASS();
}

void test_rpc_client_create() {
    TEST_START();
    
    metrics_registry_t* metrics;
    logger_t* logger;
    tcp_pool_t* pool;
    
    ASSERT_OK(metrics_init(&metrics));
    ASSERT_OK(log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC));
    ASSERT_OK(tcp_pool_create(10, metrics, logger, &pool));
    
    /* Create RPC client */
    rpc_client_t* client;
    ASSERT_OK(rpc_client_create(pool, metrics, logger, NULL, &client));
    ASSERT_TRUE(client != NULL);
    
    /* Cleanup */
    rpc_client_destroy(client);
    tcp_pool_destroy(pool);
    log_destroy(logger);
    metrics_destroy(metrics);
    
    TEST_PASS();
}

void test_rpc_echo_call() {
    TEST_START();
    
    /* Setup server */
    metrics_registry_t* metrics;
    logger_t* logger;
    tcp_server_t* tcp_server;
    rpc_server_t* server;
    
    ASSERT_OK(metrics_init(&metrics));
    ASSERT_OK(log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC));
    ASSERT_OK(tcp_server_create("127.0.0.1", 19002, metrics, logger, &tcp_server));
    ASSERT_OK(rpc_server_create(tcp_server, metrics, logger, NULL, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                         echo_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    
    usleep(100000);  /* Wait for server to start */
    
    /* Setup client */
    tcp_pool_t* pool;
    rpc_client_t* client;
    
    ASSERT_OK(tcp_pool_create(10, metrics, logger, &pool));
    ASSERT_OK(rpc_client_create(pool, metrics, logger, NULL, &client));
    
    /* Make RPC call */
    const char* test_data = "Hello, RPC!";
    uint8_t* response = NULL;
    size_t resp_len = 0;
    
    ASSERT_OK(rpc_call(client, "127.0.0.1", 19002, MSG_CLIENT_SUBMIT,
                      (const uint8_t*)test_data, strlen(test_data),
                      &response, &resp_len, 5000));
    
    ASSERT_TRUE(response != NULL);
    ASSERT_TRUE(resp_len == strlen(test_data));
    ASSERT_TRUE(memcmp(response, test_data, resp_len) == 0);
    
    printf("  Echo successful: \"%.*s\"\n", (int)resp_len, response);
    
    free(response);
    
    /* Cleanup */
    rpc_server_stop(server);
    rpc_client_destroy(client);
    rpc_server_destroy(server);
    tcp_pool_destroy(pool);
    tcp_server_destroy(tcp_server);
    log_destroy(logger);
    metrics_destroy(metrics);
    
    TEST_PASS();
}

void test_rpc_raft_request_vote() {
    TEST_START();
    
    /* Setup server */
    metrics_registry_t* metrics;
    logger_t* logger;
    tcp_server_t* tcp_server;
    rpc_server_t* server;
    
    ASSERT_OK(metrics_init(&metrics));
    ASSERT_OK(log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC));
    ASSERT_OK(tcp_server_create("127.0.0.1", 19003, metrics, logger, &tcp_server));
    ASSERT_OK(rpc_server_create(tcp_server, metrics, logger, NULL, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_RAFT_REQUEST_VOTE,
                                         request_vote_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    
    usleep(100000);
    
    /* Setup client */
    tcp_pool_t* pool;
    rpc_client_t* client;
    
    ASSERT_OK(tcp_pool_create(10, metrics, logger, &pool));
    ASSERT_OK(rpc_client_create(pool, metrics, logger, NULL, &client));
    
    /* Build request */
    raft_request_vote_t request = {
        .term = 42,
        .last_log_index = 100,
        .last_log_term = 41
    };
    strncpy(request.candidate_id, "candidate-1", sizeof(request.candidate_id) - 1);
    
    uint8_t* req_buf = NULL;
    size_t req_len = 0;
    ASSERT_OK(serialize_raft_request_vote(&request, &req_buf, &req_len));
    
    /* Make RPC call */
    uint8_t* response = NULL;
    size_t resp_len = 0;
    
    ASSERT_OK(rpc_call(client, "127.0.0.1", 19003, MSG_RAFT_REQUEST_VOTE,
                      req_buf, req_len, &response, &resp_len, 5000));
    
    /* Deserialize response */
    raft_request_vote_response_t resp;
    ASSERT_OK(deserialize_raft_request_vote_response(response, resp_len, &resp));
    
    ASSERT_TRUE(resp.vote_granted == true);
    ASSERT_TRUE(resp.term == 42);
    
    printf("  RequestVote RPC successful: vote_granted=%d, node=%s\n",
           resp.vote_granted, resp.node_id);
    
    free(req_buf);
    free(response);
    
    /* Cleanup */
    rpc_server_stop(server);
    rpc_client_destroy(client);
    rpc_server_destroy(server);
    tcp_pool_destroy(pool);
    tcp_server_destroy(tcp_server);
    log_destroy(logger);
    metrics_destroy(metrics);
    
    TEST_PASS();
}

void test_rpc_multiple_calls() {
    TEST_START();
    
    /* Setup */
    metrics_registry_t* metrics;
    logger_t* logger;
    tcp_server_t* tcp_server;
    rpc_server_t* server;
    tcp_pool_t* pool;
    rpc_client_t* client;
    
    ASSERT_OK(metrics_init(&metrics));
    ASSERT_OK(log_init(&logger, STDOUT_FILENO, LOG_MODE_SYNC));
    ASSERT_OK(tcp_server_create("127.0.0.1", 19004, metrics, logger, &tcp_server));
    ASSERT_OK(rpc_server_create(tcp_server, metrics, logger, NULL, &server));
    ASSERT_OK(rpc_server_register_handler(server, MSG_CLIENT_SUBMIT,
                                         echo_handler, NULL));
    ASSERT_OK(rpc_server_start(server));
    
    usleep(100000);
    
    ASSERT_OK(tcp_pool_create(10, metrics, logger, &pool));
    ASSERT_OK(rpc_client_create(pool, metrics, logger, NULL, &client));
    
    /* Make 10 RPC calls */
    for (int i = 0; i < 10; i++) {
        char test_data[64];
        snprintf(test_data, sizeof(test_data), "Message %d", i);
        
        uint8_t* response = NULL;
        size_t resp_len = 0;
        
        ASSERT_OK(rpc_call(client, "127.0.0.1", 19004, MSG_CLIENT_SUBMIT,
                          (const uint8_t*)test_data, strlen(test_data),
                          &response, &resp_len, 5000));
        
        ASSERT_TRUE(memcmp(response, test_data, resp_len) == 0);
        free(response);
    }
    
    printf("  10 RPC calls successful\n");
    
    /* Cleanup */
    rpc_server_stop(server);
    rpc_client_destroy(client);
    rpc_server_destroy(server);
    tcp_pool_destroy(pool);
    tcp_server_destroy(tcp_server);
    log_destroy(logger);
    metrics_destroy(metrics);
    
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Protocol - RPC Framework Tests ===\n");
    
    test_rpc_server_create();
    test_rpc_server_register_handler();
    test_rpc_client_create();
    test_rpc_echo_call();
    test_rpc_raft_request_vote();
    test_rpc_multiple_calls();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All RPC framework tests passed!\n");
        printf("✓ Session 2.4 COMPLETE\n");
        printf("✓ Phase 2 (Protocol Layer) COMPLETE\n");
        printf("✓ Ready for Phase 3 (Raft Consensus)\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}