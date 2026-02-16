/**
 * @file test_config_manager.c
 * @brief Unit tests for Configuration Manager
 */

#include <distric_cluster/config_manager.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

/* ============================================================================
 * TEST FRAMEWORK MACROS
 * ========================================================================= */

#define TEST_PASS() do { \
    printf("  ✓ PASS\n"); \
    return 0; \
} while(0)

#define TEST_FAIL(msg) do { \
    printf("  ✗ FAIL: %s\n", msg); \
    return -1; \
} while(0)

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  ✗ FAIL: Assertion failed: %s (line %d)\n", #cond, __LINE__); \
        return -1; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    if ((expected) != (actual)) { \
        printf("  ✗ FAIL: Expected %ld, got %ld (line %d)\n", \
               (long)(expected), (long)(actual), __LINE__); \
        return -1; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  ✗ FAIL: Pointer is NULL (line %d)\n", __LINE__); \
        return -1; \
    } \
} while(0)

#define TEST_ASSERT_STRING_EQUAL(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        printf("  ✗ FAIL: Expected '%s', got '%s' (line %d)\n", \
               (expected), (actual), __LINE__); \
        return -1; \
    } \
} while(0)

#define RUN_TEST(test_func) do { \
    printf("Running %s...\n", #test_func); \
    int result = test_func(); \
    if (result != 0) { \
        printf("\n✗ Test suite failed\n"); \
        return 1; \
    } \
} while(0)

/* ============================================================================
 * MOCK RAFT NODE
 * ========================================================================= */

/**
 * @brief Mock Raft node for testing
 */
typedef struct {
    bool is_leader;
    uint64_t next_index;
    
    /* Stored log entries */
    uint8_t** log_entries;
    size_t* log_sizes;
    size_t log_count;
    size_t log_capacity;
    
    /* Apply function */
    void (*apply_fn)(const uint8_t*, size_t, void*);
    void* apply_ctx;
} mock_raft_node_t;

static mock_raft_node_t* mock_raft_create(bool is_leader) {
    mock_raft_node_t* raft = calloc(1, sizeof(mock_raft_node_t));
    if (!raft) return NULL;
    
    raft->is_leader = is_leader;
    raft->next_index = 1;
    raft->log_capacity = 16;
    raft->log_entries = calloc(raft->log_capacity, sizeof(uint8_t*));
    raft->log_sizes = calloc(raft->log_capacity, sizeof(size_t));
    
    return raft;
}

static void mock_raft_destroy(mock_raft_node_t* raft) {
    if (!raft) return;
    
    for (size_t i = 0; i < raft->log_count; i++) {
        free(raft->log_entries[i]);
    }
    free(raft->log_entries);
    free(raft->log_sizes);
    free(raft);
}

/* Mock Raft API functions */
bool raft_is_leader(const raft_node_t* node) {
    const mock_raft_node_t* raft = (const mock_raft_node_t*)node;
    return raft->is_leader;
}

distric_err_t raft_append_entry(raft_node_t* node, const uint8_t* data, size_t len,
                                uint32_t* index_out) {
    mock_raft_node_t* raft = (mock_raft_node_t*)node;
    
    if (!raft->is_leader) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    /* Expand if needed */
    if (raft->log_count >= raft->log_capacity) {
        size_t new_capacity = raft->log_capacity * 2;
        uint8_t** new_entries = realloc(raft->log_entries,
                                       new_capacity * sizeof(uint8_t*));
        size_t* new_sizes = realloc(raft->log_sizes,
                                    new_capacity * sizeof(size_t));
        if (!new_entries || !new_sizes) {
            return DISTRIC_ERR_ALLOC_FAILURE;
        }
        raft->log_entries = new_entries;
        raft->log_sizes = new_sizes;
        raft->log_capacity = new_capacity;
    }
    
    /* Copy log entry */
    raft->log_entries[raft->log_count] = malloc(len);
    if (!raft->log_entries[raft->log_count]) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    memcpy(raft->log_entries[raft->log_count], data, len);
    raft->log_sizes[raft->log_count] = len;
    
    uint32_t index = (uint32_t)(raft->next_index++);
    raft->log_count++;
    
    if (index_out) {
        *index_out = index;
    }
    
    /* Immediately apply (simulating instant commit for testing) */
    if (raft->apply_fn) {
        raft->apply_fn(data, len, raft->apply_ctx);
    }
    
    return DISTRIC_OK;
}

distric_err_t raft_wait_committed(raft_node_t* node, uint32_t log_index, uint32_t timeout_ms) {
    /* For mock, we commit immediately, so always return success */
    (void)node;
    (void)log_index;
    (void)timeout_ms;
    return DISTRIC_OK;
}

/* ============================================================================
 * TEST FIXTURES
 * ========================================================================= */

typedef struct {
    mock_raft_node_t* raft;
    config_manager_t* manager;
} test_fixture_t;

static test_fixture_t* setup_fixture(bool is_leader) {
    test_fixture_t* fixture = calloc(1, sizeof(test_fixture_t));
    if (!fixture) return NULL;
    
    fixture->raft = mock_raft_create(is_leader);
    if (!fixture->raft) {
        free(fixture);
        return NULL;
    }
    
    fixture->manager = config_manager_create((raft_node_t*)fixture->raft,
                                            NULL, NULL);
    if (!fixture->manager) {
        mock_raft_destroy(fixture->raft);
        free(fixture);
        return NULL;
    }
    
    /* Manually set the apply function for testing */
    fixture->raft->apply_fn = config_manager_get_apply_function(fixture->manager);
    fixture->raft->apply_ctx = fixture->manager;
    
    config_manager_start(fixture->manager);
    
    return fixture;
}

static void teardown_fixture(test_fixture_t* fixture) {
    if (!fixture) return;
    
    config_manager_stop(fixture->manager);
    config_manager_destroy(fixture->manager);
    mock_raft_destroy(fixture->raft);
    free(fixture);
}

/* ============================================================================
 * TESTS
 * ========================================================================= */

/* Test: Create and destroy configuration manager */
static int test_create_destroy(void) {
    mock_raft_node_t* raft = mock_raft_create(true);
    TEST_ASSERT_NOT_NULL(raft);
    
    config_manager_t* manager = config_manager_create((raft_node_t*)raft,
                                                      NULL, NULL);
    TEST_ASSERT_NOT_NULL(manager);
    
    /* Verify initial state */
    uint64_t version = config_manager_get_version(manager);
    TEST_ASSERT_EQUAL(0, version);
    
    config_manager_destroy(manager);
    mock_raft_destroy(raft);
    
    TEST_PASS();
}

/* Test: Start and stop */
static int test_start_stop(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Manager is already started in setup */
    int result = config_manager_stop(fixture->manager);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Restart */
    result = config_manager_start(fixture->manager);
    TEST_ASSERT_EQUAL(0, result);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Add node configuration */
static int test_add_node(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Prepare node entry */
    config_change_t change = {
        .operation = CONFIG_OP_ADD_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_WORKER,
            .port = 8080,
            .enabled = true
        }
    };
    strncpy(change.node.node_id, "worker-1", sizeof(change.node.node_id) - 1);
    strncpy(change.node.address, "192.168.1.10",
           sizeof(change.node.address) - 1);
    
    /* Propose change */
    uint32_t log_index = 0;
    int result = config_manager_propose_change(fixture->manager, &change,
                                              &log_index);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT(log_index > 0);
    
    /* Wait for commit */
    result = config_manager_wait_committed(fixture->manager, log_index, 1000);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Verify node was added */
    config_node_entry_t node;
    result = config_manager_get_node(fixture->manager, "worker-1", &node);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(CONFIG_NODE_ROLE_WORKER, node.role);
    TEST_ASSERT_EQUAL(8080, node.port);
    TEST_ASSERT_EQUAL(true, node.enabled);
    TEST_ASSERT_STRING_EQUAL("192.168.1.10", node.address);
    
    /* Verify version incremented */
    uint32_t version = config_manager_get_version(fixture->manager);
    TEST_ASSERT_EQUAL(1, version);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Update node configuration */
static int test_update_node(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Add a node first */
    config_change_t change = {
        .operation = CONFIG_OP_ADD_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_WORKER,
            .port = 8080,
            .enabled = true
        }
    };
    strncpy(change.node.node_id, "worker-1", sizeof(change.node.node_id) - 1);
    strncpy(change.node.address, "192.168.1.10",
           sizeof(change.node.address) - 1);
    
    config_manager_propose_change(fixture->manager, &change, NULL);
    
    /* Update the node */
    change.operation = CONFIG_OP_UPDATE_NODE;
    change.node.port = 9090;
    change.node.enabled = false;
    strncpy(change.node.address, "192.168.1.20",
           sizeof(change.node.address) - 1);
    
    int result = config_manager_propose_change(fixture->manager, &change, NULL);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Verify node was updated */
    config_node_entry_t node;
    result = config_manager_get_node(fixture->manager, "worker-1", &node);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(9090, node.port);
    TEST_ASSERT_EQUAL(false, node.enabled);
    TEST_ASSERT_STRING_EQUAL("192.168.1.20", node.address);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Remove node configuration */
static int test_remove_node(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Add a node first */
    config_change_t change = {
        .operation = CONFIG_OP_ADD_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_WORKER,
            .port = 8080,
            .enabled = true
        }
    };
    strncpy(change.node.node_id, "worker-1", sizeof(change.node.node_id) - 1);
    strncpy(change.node.address, "192.168.1.10",
           sizeof(change.node.address) - 1);
    
    config_manager_propose_change(fixture->manager, &change, NULL);
    
    /* Verify node exists */
    config_node_entry_t node;
    int result = config_manager_get_node(fixture->manager, "worker-1", &node);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Remove the node */
    change.operation = CONFIG_OP_REMOVE_NODE;
    result = config_manager_propose_change(fixture->manager, &change, NULL);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Verify node was removed */
    result = config_manager_get_node(fixture->manager, "worker-1", &node);
    TEST_ASSERT_EQUAL(-1, result);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Get nodes by role */
static int test_get_nodes_by_role(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Add multiple nodes */
    for (int i = 0; i < 5; i++) {
        config_change_t change = {
            .operation = CONFIG_OP_ADD_NODE,
            .node = {
                .role = (i < 3) ? CONFIG_NODE_ROLE_WORKER :
                                 CONFIG_NODE_ROLE_COORDINATOR,
                .port = (uint16_t)(8080 + i),
                .enabled = true
            }
        };
        snprintf(change.node.node_id, sizeof(change.node.node_id),
                "node-%d", i);
        snprintf(change.node.address, sizeof(change.node.address),
                "192.168.1.%d", 10 + i);
        
        config_manager_propose_change(fixture->manager, &change, NULL);
    }
    
    /* Get worker nodes */
    config_node_entry_t* workers = NULL;
    size_t worker_count = 0;
    int result = config_manager_get_nodes_by_role(fixture->manager,
                                                  CONFIG_NODE_ROLE_WORKER,
                                                  &workers, &worker_count);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(3, worker_count);
    TEST_ASSERT_NOT_NULL(workers);
    
    /* Verify worker nodes */
    for (size_t i = 0; i < worker_count; i++) {
        TEST_ASSERT_EQUAL(CONFIG_NODE_ROLE_WORKER, workers[i].role);
    }
    free(workers);
    
    /* Get coordinator nodes */
    config_node_entry_t* coordinators = NULL;
    size_t coordinator_count = 0;
    result = config_manager_get_nodes_by_role(fixture->manager,
                                             CONFIG_NODE_ROLE_COORDINATOR,
                                             &coordinators,
                                             &coordinator_count);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, coordinator_count);
    TEST_ASSERT_NOT_NULL(coordinators);
    free(coordinators);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Set and get parameters */
static int test_set_get_param(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Set a parameter */
    config_change_t change = {
        .operation = CONFIG_OP_SET_PARAM
    };
    strncpy(change.param.key, "timeout_ms", sizeof(change.param.key) - 1);
    strncpy(change.param.value, "5000", sizeof(change.param.value) - 1);
    strncpy(change.param.description, "Timeout in milliseconds",
           sizeof(change.param.description) - 1);
    
    int result = config_manager_propose_change(fixture->manager, &change, NULL);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Get the parameter */
    char value[256];
    result = config_manager_get_param(fixture->manager, "timeout_ms",
                                     value, sizeof(value));
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_STRING_EQUAL("5000", value);
    
    /* Get as integer */
    int64_t value_int = config_manager_get_param_int(fixture->manager,
                                                     "timeout_ms", -1);
    TEST_ASSERT_EQUAL(5000, value_int);
    
    /* Get non-existent parameter */
    result = config_manager_get_param(fixture->manager, "nonexistent",
                                     value, sizeof(value));
    TEST_ASSERT_EQUAL(-1, result);
    
    /* Get non-existent parameter as int (should return default) */
    value_int = config_manager_get_param_int(fixture->manager,
                                            "nonexistent", 999);
    TEST_ASSERT_EQUAL(999, value_int);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Delete parameter */
static int test_delete_param(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Set a parameter */
    config_change_t change = {
        .operation = CONFIG_OP_SET_PARAM
    };
    strncpy(change.param.key, "temp_param", sizeof(change.param.key) - 1);
    strncpy(change.param.value, "test", sizeof(change.param.value) - 1);
    
    config_manager_propose_change(fixture->manager, &change, NULL);
    
    /* Verify it exists */
    char value[256];
    int result = config_manager_get_param(fixture->manager, "temp_param",
                                         value, sizeof(value));
    TEST_ASSERT_EQUAL(0, result);
    
    /* Delete the parameter */
    change.operation = CONFIG_OP_DELETE_PARAM;
    result = config_manager_propose_change(fixture->manager, &change, NULL);
    TEST_ASSERT_EQUAL(0, result);
    
    /* Verify it's gone */
    result = config_manager_get_param(fixture->manager, "temp_param",
                                     value, sizeof(value));
    TEST_ASSERT_EQUAL(-1, result);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Get state copy */
static int test_get_state_copy(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Add some nodes and parameters */
    config_change_t change = {
        .operation = CONFIG_OP_ADD_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_WORKER,
            .port = 8080,
            .enabled = true
        }
    };
    strncpy(change.node.node_id, "worker-1", sizeof(change.node.node_id) - 1);
    config_manager_propose_change(fixture->manager, &change, NULL);
    
    change.operation = CONFIG_OP_SET_PARAM;
    strncpy(change.param.key, "test_key", sizeof(change.param.key) - 1);
    strncpy(change.param.value, "test_value", sizeof(change.param.value) - 1);
    config_manager_propose_change(fixture->manager, &change, NULL);
    
    /* Get state copy */
    config_state_t* state = NULL;
    int result = config_manager_get_state(fixture->manager, &state);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_NOT_NULL(state);
    
    /* Verify state contents */
    TEST_ASSERT_EQUAL(2, state->version);
    TEST_ASSERT_EQUAL(1, state->node_count);
    TEST_ASSERT_EQUAL(1, state->param_count);
    
    /* Verify node */
    TEST_ASSERT_STRING_EQUAL("worker-1", state->nodes[0].node_id);
    TEST_ASSERT_EQUAL(CONFIG_NODE_ROLE_WORKER, state->nodes[0].role);
    
    /* Verify parameter */
    TEST_ASSERT_STRING_EQUAL("test_key", state->params[0].key);
    TEST_ASSERT_STRING_EQUAL("test_value", state->params[0].value);
    
    /* Free state copy */
    config_state_free(state);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Non-leader cannot propose changes */
static int test_non_leader_cannot_propose(void) {
    test_fixture_t* fixture = setup_fixture(false);  /* Not leader */
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Try to propose a change */
    config_change_t change = {
        .operation = CONFIG_OP_ADD_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_WORKER,
            .port = 8080,
            .enabled = true
        }
    };
    strncpy(change.node.node_id, "worker-1", sizeof(change.node.node_id) - 1);
    
    int result = config_manager_propose_change(fixture->manager, &change, NULL);
    TEST_ASSERT_EQUAL(-2, result);  /* Should return "not leader" error */
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* Test: Serialization and deserialization */
static int test_serialization(void) {
    /* Test node operation */
    config_change_t change = {
        .operation = CONFIG_OP_ADD_NODE,
        .node = {
            .role = CONFIG_NODE_ROLE_COORDINATOR,
            .port = 9090,
            .enabled = true
        }
    };
    strncpy(change.node.node_id, "coord-1", sizeof(change.node.node_id) - 1);
    strncpy(change.node.address, "10.0.0.1", sizeof(change.node.address) - 1);
    
    /* Serialize */
    uint8_t* data = NULL;
    size_t size = 0;
    int result = config_change_serialize(&change, &data, &size);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT(size > 0);
    
    /* Deserialize */
    config_change_t deserialized;
    result = config_change_deserialize(data, size, &deserialized);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(CONFIG_OP_ADD_NODE, deserialized.operation);
    TEST_ASSERT_STRING_EQUAL("coord-1", deserialized.node.node_id);
    TEST_ASSERT_EQUAL(CONFIG_NODE_ROLE_COORDINATOR, deserialized.node.role);
    TEST_ASSERT_EQUAL(9090, deserialized.node.port);
    TEST_ASSERT_STRING_EQUAL("10.0.0.1", deserialized.node.address);
    
    free(data);
    
    /* Test parameter operation */
    config_change_t param_change = {
        .operation = CONFIG_OP_SET_PARAM
    };
    strncpy(param_change.param.key, "max_connections",
           sizeof(param_change.param.key) - 1);
    strncpy(param_change.param.value, "1000",
           sizeof(param_change.param.value) - 1);
    
    data = NULL;
    size = 0;
    result = config_change_serialize(&param_change, &data, &size);
    TEST_ASSERT_EQUAL(0, result);
    
    config_change_t param_deserialized;
    result = config_change_deserialize(data, size, &param_deserialized);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(CONFIG_OP_SET_PARAM, param_deserialized.operation);
    TEST_ASSERT_STRING_EQUAL("max_connections", param_deserialized.param.key);
    TEST_ASSERT_STRING_EQUAL("1000", param_deserialized.param.value);
    
    free(data);
    
    TEST_PASS();
}

/* Test: Node role string conversion */
static int test_node_role_string_conversion(void) {
    /* Test to_string */
    const char* str = config_node_role_to_string(CONFIG_NODE_ROLE_COORDINATOR);
    TEST_ASSERT_STRING_EQUAL("coordinator", str);
    
    str = config_node_role_to_string(CONFIG_NODE_ROLE_WORKER);
    TEST_ASSERT_STRING_EQUAL("worker", str);
    
    /* Test from_string */
    config_node_role_t role;
    int result = config_node_role_from_string("coordinator", &role);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(CONFIG_NODE_ROLE_COORDINATOR, role);
    
    result = config_node_role_from_string("worker", &role);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(CONFIG_NODE_ROLE_WORKER, role);
    
    /* Test invalid string */
    result = config_node_role_from_string("invalid", &role);
    TEST_ASSERT_EQUAL(-1, result);
    
    TEST_PASS();
}

/* Test: Concurrent reads */
typedef struct {
    config_manager_t* manager;
    int thread_id;
    int iterations;
    int errors;
} thread_arg_t;

static void* reader_thread(void* arg) {
    thread_arg_t* args = (thread_arg_t*)arg;
    
    for (int i = 0; i < args->iterations; i++) {
        config_state_t* state = NULL;
        int result = config_manager_get_state(args->manager, &state);
        if (result != 0) {
            args->errors++;
            continue;
        }
        
        /* Verify state is valid */
        if (state->version > 1000) {  /* Sanity check */
            args->errors++;
        }
        
        config_state_free(state);
        
        /* Small delay */
        usleep(100);
    }
    
    return NULL;
}

static int test_concurrent_reads(void) {
    test_fixture_t* fixture = setup_fixture(true);
    TEST_ASSERT_NOT_NULL(fixture);
    
    /* Add some data */
    for (int i = 0; i < 10; i++) {
        config_change_t change = {
            .operation = CONFIG_OP_ADD_NODE,
            .node = {
                .role = CONFIG_NODE_ROLE_WORKER,
                .port = (uint16_t)(8080 + i),
                .enabled = true
            }
        };
        snprintf(change.node.node_id, sizeof(change.node.node_id), "node-%d", i);
        config_manager_propose_change(fixture->manager, &change, NULL);
    }
    
    /* Start multiple reader threads */
    const int NUM_THREADS = 4;
    const int ITERATIONS = 100;
    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].manager = fixture->manager;
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].errors = 0;
        
        pthread_create(&threads[i], NULL, reader_thread, &args[i]);
    }
    
    /* Wait for all threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Verify no errors */
    int total_errors = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_errors += args[i].errors;
    }
    TEST_ASSERT_EQUAL(0, total_errors);
    
    teardown_fixture(fixture);
    TEST_PASS();
}

/* ============================================================================
 * TEST RUNNER
 * ========================================================================= */

int main(void) {
    printf("=============================================================================\n");
    printf("  Configuration Manager Unit Tests\n");
    printf("=============================================================================\n\n");
    
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_start_stop);
    RUN_TEST(test_add_node);
    RUN_TEST(test_update_node);
    RUN_TEST(test_remove_node);
    RUN_TEST(test_get_nodes_by_role);
    RUN_TEST(test_set_get_param);
    RUN_TEST(test_delete_param);
    RUN_TEST(test_get_state_copy);
    RUN_TEST(test_non_leader_cannot_propose);
    RUN_TEST(test_serialization);
    RUN_TEST(test_node_role_string_conversion);
    RUN_TEST(test_concurrent_reads);
    
    printf("\n=============================================================================\n");
    printf("  ALL TESTS PASSED (13/13)\n");
    printf("=============================================================================\n\n");
    
    return 0;
}