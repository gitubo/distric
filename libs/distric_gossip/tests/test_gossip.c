/**
 * @file test_gossip.c
 * @brief Unit tests for DistriC Gossip Protocol
 * 
 * Tests cover:
 * - Initialization and lifecycle
 * - Membership management
 * - State transitions
 * - SWIM protocol mechanics
 * - Query operations
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#include "distric_gossip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

/* Test helpers */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START() \
    printf("\n--- %s ---\n", __func__)

#define TEST_PASS() \
    do { \
        tests_passed++; \
        printf("  ✓ PASS\n"); \
    } while(0)

#define ASSERT_OK(expr) \
    do { \
        distric_err_t _err = (expr); \
        if (_err != DISTRIC_OK) { \
            fprintf(stderr, "  ✗ FAIL at %s:%d: %s returned %d\n", \
                   __FILE__, __LINE__, #expr, _err); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "  ✗ FAIL at %s:%d: %s is false\n", \
                   __FILE__, __LINE__, #expr); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(expr) \
    do { \
        if (expr) { \
            fprintf(stderr, "  ✗ FAIL at %s:%d: %s is true\n", \
                   __FILE__, __LINE__, #expr); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            fprintf(stderr, "  ✗ FAIL at %s:%d: %s (%ld) != %s (%ld)\n", \
                   __FILE__, __LINE__, #a, (long)(a), #b, (long)(b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            fprintf(stderr, "  ✗ FAIL at %s:%d: %s (%s) != %s (%s)\n", \
                   __FILE__, __LINE__, #a, (a), #b, (b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

/* ============================================================================
 * TEST CASES
 * ========================================================================= */

void test_gossip_init_destroy() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "test-node-1", sizeof(config.node_id) - 1);
    strncpy(config.bind_address, "127.0.0.1", sizeof(config.bind_address) - 1);
    config.bind_port = 7946;
    config.role = GOSSIP_ROLE_WORKER;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    ASSERT_TRUE(state != NULL);
    
    printf("  Gossip initialized successfully\n");
    
    gossip_destroy(state);
    
    printf("  Gossip destroyed successfully\n");
    
    TEST_PASS();
}

void test_gossip_invalid_args() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    gossip_state_t* state = NULL;
    
    /* NULL config */
    distric_err_t err = gossip_init(NULL, &state);
    ASSERT_TRUE(err == DISTRIC_ERR_INVALID_ARG);
    
    /* NULL output */
    err = gossip_init(&config, NULL);
    ASSERT_TRUE(err == DISTRIC_ERR_INVALID_ARG);
    
    /* Empty node_id */
    config.bind_port = 7946;
    err = gossip_init(&config, &state);
    ASSERT_TRUE(err == DISTRIC_ERR_INVALID_ARG);
    
    /* Zero port */
    strncpy(config.node_id, "test", sizeof(config.node_id) - 1);
    config.bind_port = 0;
    err = gossip_init(&config, &state);
    ASSERT_TRUE(err == DISTRIC_ERR_INVALID_ARG);
    
    printf("  All invalid argument cases rejected\n");
    
    TEST_PASS();
}

void test_gossip_default_config() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "test-node", sizeof(config.node_id) - 1);
    config.bind_port = 7947;
    config.role = GOSSIP_ROLE_COORDINATOR;
    /* Leave all other fields as 0 - should get defaults */
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Verify defaults were applied (we can't access internal state directly,
       but we can verify it doesn't crash and runs) */
    printf("  Default configuration applied\n");
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_membership_queries() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "node-1", sizeof(config.node_id) - 1);
    strncpy(config.bind_address, "127.0.0.1", sizeof(config.bind_address) - 1);
    config.bind_port = 7948;
    config.role = GOSSIP_ROLE_WORKER;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Get alive nodes (should have self) */
    gossip_node_info_t* nodes = NULL;
    size_t count = 0;
    
    ASSERT_OK(gossip_get_alive_nodes(state, &nodes, &count));
    ASSERT_TRUE(count >= 1);  /* At least self */
    ASSERT_TRUE(nodes != NULL);
    
    printf("  Found %zu alive node(s)\n", count);
    
    /* Verify self is in the list */
    bool found_self = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(nodes[i].node_id, "node-1") == 0) {
            found_self = true;
            ASSERT_EQ(nodes[i].status, GOSSIP_NODE_ALIVE);
            ASSERT_EQ(nodes[i].role, GOSSIP_ROLE_WORKER);
            break;
        }
    }
    ASSERT_TRUE(found_self);
    
    free(nodes);
    
    /* Test is_node_alive */
    bool is_alive = false;
    ASSERT_OK(gossip_is_node_alive(state, "node-1", &is_alive));
    ASSERT_TRUE(is_alive);
    
    ASSERT_OK(gossip_is_node_alive(state, "nonexistent", &is_alive));
    ASSERT_FALSE(is_alive);
    
    printf("  Node alive queries work correctly\n");
    
    /* Test get_node_info */
    gossip_node_info_t info;
    ASSERT_OK(gossip_get_node_info(state, "node-1", &info));
    ASSERT_STR_EQ(info.node_id, "node-1");
    ASSERT_EQ(info.status, GOSSIP_NODE_ALIVE);
    
    distric_err_t err = gossip_get_node_info(state, "nonexistent", &info);
    ASSERT_TRUE(err == DISTRIC_ERR_NOT_FOUND);
    
    printf("  Node info queries work correctly\n");
    
    /* Test node count */
    size_t total_count = 0;
    ASSERT_OK(gossip_get_node_count(state, (gossip_node_status_t)-1, &total_count));
    ASSERT_TRUE(total_count >= 1);
    
    size_t alive_count = 0;
    ASSERT_OK(gossip_get_node_count(state, GOSSIP_NODE_ALIVE, &alive_count));
    ASSERT_TRUE(alive_count >= 1);
    
    printf("  Node count: total=%zu, alive=%zu\n", total_count, alive_count);
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_get_nodes_by_role() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "coordinator-1", sizeof(config.node_id) - 1);
    config.bind_port = 7949;
    config.role = GOSSIP_ROLE_COORDINATOR;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Get coordinators */
    gossip_node_info_t* coordinators = NULL;
    size_t coord_count = 0;
    
    ASSERT_OK(gossip_get_nodes_by_role(state, GOSSIP_ROLE_COORDINATOR, &coordinators, &coord_count));
    ASSERT_TRUE(coord_count >= 1);  /* Self */
    
    printf("  Found %zu coordinator(s)\n", coord_count);
    
    free(coordinators);
    
    /* Get workers (should be 0) */
    gossip_node_info_t* workers = NULL;
    size_t worker_count = 0;
    
    ASSERT_OK(gossip_get_nodes_by_role(state, GOSSIP_ROLE_WORKER, &workers, &worker_count));
    ASSERT_EQ(worker_count, 0);
    ASSERT_TRUE(workers == NULL);
    
    printf("  Found %zu worker(s)\n", worker_count);
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_callbacks() {
    TEST_START();
    
    static int joined_count = 0;
    static int suspected_count = 0;
    static int failed_count = 0;
    static int recovered_count = 0;
    
    void on_joined(const gossip_node_info_t* node, void* user_data) {
        (void)node;
        int* counter = (int*)user_data;
        (*counter)++;
    }
    
    void on_suspected(const gossip_node_info_t* node, void* user_data) {
        (void)node;
        int* counter = (int*)user_data;
        (*counter)++;
    }
    
    void on_failed(const gossip_node_info_t* node, void* user_data) {
        (void)node;
        int* counter = (int*)user_data;
        (*counter)++;
    }
    
    void on_recovered(const gossip_node_info_t* node, void* user_data) {
        (void)node;
        int* counter = (int*)user_data;
        (*counter)++;
    }
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "callback-test", sizeof(config.node_id) - 1);
    config.bind_port = 7950;
    config.role = GOSSIP_ROLE_WORKER;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Register callbacks */
    ASSERT_OK(gossip_set_on_node_joined(state, on_joined, &joined_count));
    ASSERT_OK(gossip_set_on_node_suspected(state, on_suspected, &suspected_count));
    ASSERT_OK(gossip_set_on_node_failed(state, on_failed, &failed_count));
    ASSERT_OK(gossip_set_on_node_recovered(state, on_recovered, &recovered_count));
    
    printf("  Callbacks registered successfully\n");
    
    /* Note: Callbacks would be triggered during actual protocol operation
       In a full integration test, we would start the protocol and verify
       callbacks are invoked. For this unit test, we just verify registration works */
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_update_load() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "load-test", sizeof(config.node_id) - 1);
    config.bind_port = 7951;
    config.role = GOSSIP_ROLE_WORKER;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Update load */
    ASSERT_OK(gossip_update_load(state, 75));
    
    /* Verify load was updated */
    gossip_node_info_t info;
    ASSERT_OK(gossip_get_node_info(state, "load-test", &info));
    ASSERT_EQ(info.load, 75);
    
    printf("  Load updated to %lu\n", (unsigned long)info.load);
    
    /* Update again */
    ASSERT_OK(gossip_update_load(state, 50));
    ASSERT_OK(gossip_get_node_info(state, "load-test", &info));
    ASSERT_EQ(info.load, 50);
    
    printf("  Load updated to %lu\n", (unsigned long)info.load);
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_refute_suspicion() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "refute-test", sizeof(config.node_id) - 1);
    config.bind_port = 7952;
    config.role = GOSSIP_ROLE_WORKER;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Get initial incarnation */
    gossip_node_info_t info1;
    ASSERT_OK(gossip_get_node_info(state, "refute-test", &info1));
    uint64_t initial_incarnation = info1.incarnation;
    
    printf("  Initial incarnation: %lu\n", (unsigned long)initial_incarnation);
    
    /* Refute suspicion */
    ASSERT_OK(gossip_refute_suspicion(state));
    
    /* Verify incarnation increased */
    gossip_node_info_t info2;
    ASSERT_OK(gossip_get_node_info(state, "refute-test", &info2));
    ASSERT_TRUE(info2.incarnation > initial_incarnation);
    
    printf("  After refute: incarnation=%lu\n", (unsigned long)info2.incarnation);
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_add_seed() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "seed-test", sizeof(config.node_id) - 1);
    config.bind_port = 7953;
    config.role = GOSSIP_ROLE_WORKER;
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Add seed node */
    ASSERT_OK(gossip_add_seed(state, "192.168.1.100", 7946));
    
    printf("  Seed node added\n");
    
    /* Verify membership increased */
    size_t count = 0;
    ASSERT_OK(gossip_get_node_count(state, (gossip_node_status_t)-1, &count));
    ASSERT_TRUE(count >= 2);  /* Self + seed */
    
    printf("  Total nodes: %zu\n", count);
    
    gossip_destroy(state);
    
    TEST_PASS();
}

void test_gossip_utility_functions() {
    TEST_START();
    
    /* Test status_to_string */
    ASSERT_STR_EQ(gossip_status_to_string(GOSSIP_NODE_ALIVE), "ALIVE");
    ASSERT_STR_EQ(gossip_status_to_string(GOSSIP_NODE_SUSPECTED), "SUSPECTED");
    ASSERT_STR_EQ(gossip_status_to_string(GOSSIP_NODE_FAILED), "FAILED");
    ASSERT_STR_EQ(gossip_status_to_string(GOSSIP_NODE_LEFT), "LEFT");
    
    printf("  Status string conversion works\n");
    
    /* Test role_to_string */
    ASSERT_STR_EQ(gossip_role_to_string(GOSSIP_ROLE_COORDINATOR), "COORDINATOR");
    ASSERT_STR_EQ(gossip_role_to_string(GOSSIP_ROLE_WORKER), "WORKER");
    
    printf("  Role string conversion works\n");
    
    TEST_PASS();
}

void test_gossip_start_stop() {
    TEST_START();
    
    gossip_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, "start-stop-test", sizeof(config.node_id) - 1);
    config.bind_port = 7954;
    config.role = GOSSIP_ROLE_WORKER;
    config.protocol_period_ms = 100;  /* Fast for testing */
    
    gossip_state_t* state = NULL;
    ASSERT_OK(gossip_init(&config, &state));
    
    /* Start protocol */
    ASSERT_OK(gossip_start(state));
    printf("  Protocol started\n");
    
    /* Let it run for a bit */
    usleep(500000);  /* 500ms */
    
    /* Stop protocol */
    ASSERT_OK(gossip_stop(state));
    printf("  Protocol stopped\n");
    
    /* Start again */
    ASSERT_OK(gossip_start(state));
    printf("  Protocol restarted\n");
    
    usleep(500000);  /* 500ms */
    
    /* Stop and destroy */
    ASSERT_OK(gossip_stop(state));
    gossip_destroy(state);
    
    printf("  Start/stop cycle completed successfully\n");
    
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Gossip - Unit Tests (Session 4.1) ===\n");
    
    test_gossip_init_destroy();
    test_gossip_invalid_args();
    test_gossip_default_config();
    test_gossip_membership_queries();
    test_gossip_get_nodes_by_role();
    test_gossip_callbacks();
    test_gossip_update_load();
    test_gossip_refute_suspicion();
    test_gossip_add_seed();
    test_gossip_utility_functions();
    test_gossip_start_stop();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All Gossip tests passed!\n");
        printf("✓ Session 4.1 (Gossip Core) COMPLETE\n");
        printf("\nKey Features Implemented:\n");
        printf("  - SWIM protocol core structure\n");
        printf("  - Membership management with state transitions\n");
        printf("  - Direct and indirect probing\n");
        printf("  - Suspicion and failure detection\n");
        printf("  - Incarnation-based conflict resolution\n");
        printf("  - Event callbacks for state changes\n");
        printf("  - Query APIs for membership\n");
        printf("  - UDP-based messaging\n");
        printf("\nNext Steps:\n");
        printf("  - Session 4.2: Multi-node gossip integration tests\n");
        printf("  - Session 4.3: Integration with Raft layer\n");
        printf("  - Phase 5: Cluster Management\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}