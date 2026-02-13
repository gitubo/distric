/**
 * @file test_cluster_integration.c
 * @brief Comprehensive tests for Section 4.3 - Raft + Gossip Integration
 * 
 * Tests the cluster coordinator, worker pool, and integration between
 * Raft and Gossip protocols.
 * 
 * @version 1.0
 * @date 2026-02-12
 */

#include "distric_cluster.h"
#include "worker_pool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* Test utilities */
#define TEST_PASS() printf("  ✓ PASS\n")
#define TEST_FAIL(msg) do { printf("  ✗ FAIL: %s\n", msg); return -1; } while(0)
#define RUN_TEST(test_fn) do { \
    printf("Running %s...\n", #test_fn); \
    if (test_fn() != 0) { \
        printf("TEST FAILED: %s\n", #test_fn); \
        return -1; \
    } \
} while(0)

/* ============================================================================
 * TEST 1: Worker Pool Creation and Destruction
 * ========================================================================= */

static int test_worker_pool_create_destroy(void) {
    printf("  Test: Create and destroy worker pool\n");
    
    worker_pool_t* pool = NULL;
    distric_err_t err = worker_pool_create(NULL, NULL, &pool);
    
    if (err != DISTRIC_OK || !pool) {
        TEST_FAIL("Failed to create worker pool");
    }
    
    worker_pool_destroy(pool);
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 2: Worker Pool - Add and Query Workers
 * ========================================================================= */

static int test_worker_pool_add_query(void) {
    printf("  Test: Add workers and query pool state\n");
    
    worker_pool_t* pool = NULL;
    distric_err_t err = worker_pool_create(NULL, NULL, &pool);
    if (err != DISTRIC_OK) {
        TEST_FAIL("Failed to create pool");
    }
    
    /* Add workers via gossip updates */
    for (int i = 0; i < 5; i++) {
        gossip_node_info_t node;
        memset(&node, 0, sizeof(node));
        
        snprintf(node.node_id, sizeof(node.node_id), "worker-%d", i);
        snprintf(node.address, sizeof(node.address), "192.168.1.%d", 100 + i);
        node.port = 8000 + i;
        node.status = GOSSIP_NODE_ALIVE;
        node.role = GOSSIP_ROLE_WORKER;
        node.incarnation = 1;
        node.load = i * 10;
        
        err = worker_pool_update_from_gossip(pool, &node);
        if (err != DISTRIC_OK) {
            worker_pool_destroy(pool);
            TEST_FAIL("Failed to add worker");
        }
    }
    
    /* Query worker count */
    size_t count = 0;
    err = worker_pool_get_count(pool, GOSSIP_NODE_ALIVE, &count);
    if (err != DISTRIC_OK || count != 5) {
        worker_pool_destroy(pool);
        TEST_FAIL("Wrong worker count");
    }
    
    /* Get specific worker */
    worker_info_t worker;
    err = worker_pool_get_worker(pool, "worker-2", &worker);
    if (err != DISTRIC_OK) {
        worker_pool_destroy(pool);
        TEST_FAIL("Failed to get specific worker");
    }
    
    if (strcmp(worker.worker_id, "worker-2") != 0) {
        worker_pool_destroy(pool);
        TEST_FAIL("Wrong worker returned");
    }
    
    worker_pool_destroy(pool);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 3: Worker Selection Strategies
 * ========================================================================= */

static int test_worker_selection_strategies(void) {
    printf("  Test: Worker selection strategies\n");
    
    worker_pool_t* pool = NULL;
    distric_err_t err = worker_pool_create(NULL, NULL, &pool);
    if (err != DISTRIC_OK) {
        TEST_FAIL("Failed to create pool");
    }
    
    /* Add workers with different loads */
    for (int i = 0; i < 3; i++) {
        gossip_node_info_t node;
        memset(&node, 0, sizeof(node));
        
        snprintf(node.node_id, sizeof(node.node_id), "worker-%d", i);
        snprintf(node.address, sizeof(node.address), "192.168.1.%d", 100 + i);
        node.port = 8000;
        node.status = GOSSIP_NODE_ALIVE;
        node.role = GOSSIP_ROLE_WORKER;
        
        worker_pool_update_from_gossip(pool, &node);
        
        /* Set different task counts */
        for (int j = 0; j < i; j++) {
            worker_pool_update_task_count(pool, node.node_id, 1);
        }
    }
    
    /* Test LEAST_LOADED - should select worker-0 (0 tasks) */
    const worker_info_t* selected = NULL;
    err = worker_pool_get_available(pool, WORKER_SELECT_LEAST_LOADED, &selected);
    if (err != DISTRIC_OK || !selected) {
        worker_pool_destroy(pool);
        TEST_FAIL("Failed to select worker");
    }
    
    if (strcmp(selected->worker_id, "worker-0") != 0) {
        worker_pool_destroy(pool);
        TEST_FAIL("Wrong worker selected (expected least loaded)");
    }
    
    /* Test ROUND_ROBIN */
    err = worker_pool_get_available(pool, WORKER_SELECT_ROUND_ROBIN, &selected);
    if (err != DISTRIC_OK || !selected) {
        worker_pool_destroy(pool);
        TEST_FAIL("Round robin selection failed");
    }
    
    worker_pool_destroy(pool);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 4: Worker Failure Handling
 * ========================================================================= */

static int test_worker_failure_handling(void) {
    printf("  Test: Worker failure handling\n");
    
    worker_pool_t* pool = NULL;
    distric_err_t err = worker_pool_create(NULL, NULL, &pool);
    if (err != DISTRIC_OK) {
        TEST_FAIL("Failed to create pool");
    }
    
    /* Add workers */
    for (int i = 0; i < 3; i++) {
        gossip_node_info_t node;
        memset(&node, 0, sizeof(node));
        snprintf(node.node_id, sizeof(node.node_id), "worker-%d", i);
        snprintf(node.address, sizeof(node.address), "192.168.1.%d", 100 + i);
        node.port = 8000;
        node.status = GOSSIP_NODE_ALIVE;
        node.role = GOSSIP_ROLE_WORKER;
        worker_pool_update_from_gossip(pool, &node);
    }
    
    /* Mark worker-1 as failed */
    err = worker_pool_mark_failed(pool, "worker-1");
    if (err != DISTRIC_OK) {
        worker_pool_destroy(pool);
        TEST_FAIL("Failed to mark worker as failed");
    }
    
    /* Verify alive count */
    size_t alive_count = 0;
    worker_pool_get_count(pool, GOSSIP_NODE_ALIVE, &alive_count);
    if (alive_count != 2) {
        worker_pool_destroy(pool);
        TEST_FAIL("Wrong alive worker count after failure");
    }
    
    /* Verify failed worker not selected */
    for (int i = 0; i < 10; i++) {
        const worker_info_t* selected = NULL;
        err = worker_pool_get_available(pool, WORKER_SELECT_ROUND_ROBIN, &selected);
        if (err == DISTRIC_OK && selected) {
            if (strcmp(selected->worker_id, "worker-1") == 0) {
                worker_pool_destroy(pool);
                TEST_FAIL("Failed worker was selected");
            }
        }
    }
    
    worker_pool_destroy(pool);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 5: Cluster Coordinator Creation
 * ========================================================================= */

static int test_coordinator_create_destroy_worker(void) {
    printf("  Test: Create and destroy worker coordinator\n");
    
    cluster_config_t config = {
        .node_id = "worker-test",
        .node_type = CLUSTER_NODE_WORKER,
        .bind_address = "127.0.0.1",
        .gossip_port = 17946,
        .max_concurrent_tasks = 10,
        .metrics = NULL,
        .logger = NULL,
        .tracer = NULL
    };
    
    cluster_coordinator_t* coord = NULL;
    distric_err_t err = cluster_coordinator_create(&config, &coord);
    
    if (err != DISTRIC_OK || !coord) {
        TEST_FAIL("Failed to create worker coordinator");
    }
    
    cluster_coordinator_destroy(coord);
    
    TEST_PASS();
    return 0;
}

static int test_coordinator_create_destroy_coordinator(void) {
    printf("  Test: Create and destroy coordination node\n");
    
    cluster_config_t config = {
        .node_id = "coord-test",
        .node_type = CLUSTER_NODE_COORDINATOR,
        .bind_address = "127.0.0.1",
        .gossip_port = 17947,
        .raft_port = 18300,
        .raft_peer_count = 1,
        .max_concurrent_tasks = 0,
        .metrics = NULL,
        .logger = NULL,
        .tracer = NULL
    };
    
    /* Self as only peer */
    snprintf(config.raft_peers[0], sizeof(config.raft_peers[0]),
             "coord-test@127.0.0.1:18300");
    
    cluster_coordinator_t* coord = NULL;
    distric_err_t err = cluster_coordinator_create(&config, &coord);
    
    if (err != DISTRIC_OK || !coord) {
        TEST_FAIL("Failed to create coordinator");
    }
    
    cluster_coordinator_destroy(coord);
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 6: Coordinator State Queries
 * ========================================================================= */

static int test_coordinator_state_queries(void) {
    printf("  Test: Coordinator state queries\n");
    
    cluster_config_t config = {
        .node_id = "coord-query-test",
        .node_type = CLUSTER_NODE_COORDINATOR,
        .bind_address = "127.0.0.1",
        .gossip_port = 17948,
        .raft_port = 18301,
        .raft_peer_count = 1,
        .metrics = NULL,
        .logger = NULL
    };
    
    snprintf(config.raft_peers[0], sizeof(config.raft_peers[0]),
             "coord-query-test@127.0.0.1:18301");
    
    cluster_coordinator_t* coord = NULL;
    distric_err_t err = cluster_coordinator_create(&config, &coord);
    if (err != DISTRIC_OK) {
        TEST_FAIL("Failed to create coordinator");
    }
    
    /* Test state query */
    cluster_state_t state;
    err = cluster_coordinator_get_state(coord, &state);
    if (err != DISTRIC_OK || state != CLUSTER_STATE_STOPPED) {
        cluster_coordinator_destroy(coord);
        TEST_FAIL("Wrong initial state");
    }
    
    /* Test leadership query (should be false initially) */
    bool is_leader;
    err = cluster_coordinator_is_leader(coord, &is_leader);
    if (err != DISTRIC_OK || is_leader) {
        cluster_coordinator_destroy(coord);
        TEST_FAIL("Wrong initial leadership state");
    }
    
    cluster_coordinator_destroy(coord);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 7: Callbacks
 * ========================================================================= */

static bool g_leadership_callback_called = false;
static bool g_worker_callback_called = false;

static void test_on_became_leader(cluster_coordinator_t* coord, void* user_data) {
    (void)coord;
    (void)user_data;
    g_leadership_callback_called = true;
}

static void test_on_lost_leadership(cluster_coordinator_t* coord, void* user_data) {
    (void)coord;
    (void)user_data;
}

static void test_on_worker_joined(cluster_coordinator_t* coord,
                                  const cluster_node_t* worker,
                                  void* user_data) {
    (void)coord;
    (void)worker;
    (void)user_data;
    g_worker_callback_called = true;
}

static void test_on_worker_failed(cluster_coordinator_t* coord,
                                  const cluster_node_t* worker,
                                  void* user_data) {
    (void)coord;
    (void)worker;
    (void)user_data;
}

static int test_coordinator_callbacks(void) {
    printf("  Test: Coordinator callbacks\n");
    
    cluster_config_t config = {
        .node_id = "coord-callback-test",
        .node_type = CLUSTER_NODE_COORDINATOR,
        .bind_address = "127.0.0.1",
        .gossip_port = 17949,
        .raft_port = 18302,
        .raft_peer_count = 1,
        .metrics = NULL,
        .logger = NULL
    };
    
    snprintf(config.raft_peers[0], sizeof(config.raft_peers[0]),
             "coord-callback-test@127.0.0.1:18302");
    
    cluster_coordinator_t* coord = NULL;
    distric_err_t err = cluster_coordinator_create(&config, &coord);
    if (err != DISTRIC_OK) {
        TEST_FAIL("Failed to create coordinator");
    }
    
    /* Set callbacks */
    err = cluster_coordinator_set_leadership_callbacks(coord,
                                                      test_on_became_leader,
                                                      test_on_lost_leadership,
                                                      NULL);
    if (err != DISTRIC_OK) {
        cluster_coordinator_destroy(coord);
        TEST_FAIL("Failed to set leadership callbacks");
    }
    
    err = cluster_coordinator_set_worker_callbacks(coord,
                                                   test_on_worker_joined,
                                                   test_on_worker_failed,
                                                   NULL);
    if (err != DISTRIC_OK) {
        cluster_coordinator_destroy(coord);
        TEST_FAIL("Failed to set worker callbacks");
    }
    
    /* Note: Callbacks won't actually fire in this test without starting
     * the coordinator and simulating events, but we've verified the
     * registration works */
    
    cluster_coordinator_destroy(coord);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 8: Capacity Statistics
 * ========================================================================= */

static int test_worker_capacity_stats(void) {
    printf("  Test: Worker pool capacity statistics\n");
    
    worker_pool_t* pool = NULL;
    distric_err_t err = worker_pool_create(NULL, NULL, &pool);
    if (err != DISTRIC_OK) {
        TEST_FAIL("Failed to create pool");
    }
    
    /* Add workers with known capacities */
    for (int i = 0; i < 3; i++) {
        gossip_node_info_t node;
        memset(&node, 0, sizeof(node));
        snprintf(node.node_id, sizeof(node.node_id), "worker-%d", i);
        snprintf(node.address, sizeof(node.address), "192.168.1.%d", 100 + i);
        node.port = 8000;
        node.status = GOSSIP_NODE_ALIVE;
        node.role = GOSSIP_ROLE_WORKER;
        worker_pool_update_from_gossip(pool, &node);
        
        /* Assign some tasks */
        for (int j = 0; j < i; j++) {
            worker_pool_update_task_count(pool, node.node_id, 1);
        }
    }
    
    /* Get capacity stats */
    uint64_t total_capacity = 0;
    uint64_t total_utilized = 0;
    err = worker_pool_get_capacity_stats(pool, &total_capacity, &total_utilized);
    if (err != DISTRIC_OK) {
        worker_pool_destroy(pool);
        TEST_FAIL("Failed to get capacity stats");
    }
    
    /* Expected: 3 workers * 10 tasks = 30 capacity, 0+1+2 = 3 utilized */
    if (total_capacity != 30) {
        worker_pool_destroy(pool);
        TEST_FAIL("Wrong total capacity");
    }
    
    if (total_utilized != 3) {
        worker_pool_destroy(pool);
        TEST_FAIL("Wrong total utilization");
    }
    
    worker_pool_destroy(pool);
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 9: Utility Functions
 * ========================================================================= */

static int test_utility_functions(void) {
    printf("  Test: Utility functions\n");
    
    /* Test state to string */
    const char* state_str = cluster_state_to_string(CLUSTER_STATE_RUNNING);
    if (strcmp(state_str, "RUNNING") != 0) {
        TEST_FAIL("Wrong state string");
    }
    
    /* Test node type to string */
    const char* type_str = cluster_node_type_to_string(CLUSTER_NODE_WORKER);
    if (strcmp(type_str, "WORKER") != 0) {
        TEST_FAIL("Wrong node type string");
    }
    
    /* Test strategy to string */
    const char* strategy_str = worker_selection_strategy_to_string(WORKER_SELECT_LEAST_LOADED);
    if (strcmp(strategy_str, "LEAST_LOADED") != 0) {
        TEST_FAIL("Wrong strategy string");
    }
    
    /* Test version */
    const char* version = distric_cluster_version();
    if (!version || strlen(version) == 0) {
        TEST_FAIL("Invalid version string");
    }
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * MAIN TEST RUNNER
 * ========================================================================= */

int main(void) {
    printf("=======================================================\n");
    printf("  Cluster Integration Tests (Section 4.3)\n");
    printf("=======================================================\n\n");
    
    /* Test Suite 1: Worker Pool */
    printf("Suite 1: Worker Pool Management\n");
    printf("--------------------------------\n");
    RUN_TEST(test_worker_pool_create_destroy);
    RUN_TEST(test_worker_pool_add_query);
    RUN_TEST(test_worker_selection_strategies);
    RUN_TEST(test_worker_failure_handling);
    RUN_TEST(test_worker_capacity_stats);
    printf("\n");
    
    /* Test Suite 2: Cluster Coordinator */
    printf("Suite 2: Cluster Coordinator\n");
    printf("----------------------------\n");
    RUN_TEST(test_coordinator_create_destroy_worker);
    RUN_TEST(test_coordinator_create_destroy_coordinator);
    RUN_TEST(test_coordinator_state_queries);
    RUN_TEST(test_coordinator_callbacks);
    printf("\n");
    
    /* Test Suite 3: Utilities */
    printf("Suite 3: Utility Functions\n");
    printf("--------------------------\n");
    RUN_TEST(test_utility_functions);
    printf("\n");
    
    printf("=======================================================\n");
    printf("  ALL TESTS PASSED (10/10)\n");
    printf("=======================================================\n");
    printf("\n");
    printf("Note: Full integration tests with live Raft/Gossip\n");
    printf("      protocols require multi-process setup.\n");
    printf("      These tests verify API contracts and basic logic.\n");
    
    return 0;
}