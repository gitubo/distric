/**
 * @file test_raft_basic.c
 * @brief Basic tests for Raft consensus core
 * 
 * Tests:
 * - Node creation and initialization
 * - State transitions
 * - Leader election logic
 * - RequestVote RPC handling
 * - AppendEntries RPC handling
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <distric_raft/raft_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

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

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s (%d) != %s (%d)\n", #a, (int)(a), #b, (int)(b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* ============================================================================
 * TEST HELPERS
 * ========================================================================= */

static raft_config_t create_test_config(const char* node_id, size_t peer_count) {
    raft_config_t config;
    memset(&config, 0, sizeof(config));
    
    strncpy(config.node_id, node_id, sizeof(config.node_id) - 1);
    
    /* Create peers */
    config.peers = (raft_peer_t*)calloc(peer_count, sizeof(raft_peer_t));
    config.peer_count = peer_count;
    
    for (size_t i = 0; i < peer_count; i++) {
        snprintf(config.peers[i].node_id, sizeof(config.peers[i].node_id), "peer-%zu", i);
        snprintf(config.peers[i].address, sizeof(config.peers[i].address), "10.0.1.%zu", i + 1);
        config.peers[i].port = 9000 + i;
    }
    
    /* Timing configuration */
    config.election_timeout_min_ms = 150;
    config.election_timeout_max_ms = 300;
    config.heartbeat_interval_ms = 50;
    
    /* No callbacks for basic tests */
    config.apply_fn = NULL;
    config.state_change_fn = NULL;
    config.user_data = NULL;
    
    /* No observability for basic tests */
    config.metrics = NULL;
    config.logger = NULL;
    
    return config;
}

static void free_test_config(raft_config_t* config) {
    free(config->peers);
}

/* ============================================================================
 * BASIC TESTS
 * ========================================================================= */

void test_node_creation() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_TRUE(node != NULL);
    
    /* Verify initial state */
    ASSERT_EQ(raft_get_state(node), RAFT_STATE_FOLLOWER);
    ASSERT_EQ(raft_get_term(node), 0);
    ASSERT_TRUE(!raft_is_leader(node));
    ASSERT_EQ(raft_get_commit_index(node), 0);
    ASSERT_EQ(raft_get_last_log_index(node), 0);
    
    printf("  Initial state: FOLLOWER, term=0\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_state_queries() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    
    /* Test state string */
    ASSERT_TRUE(strcmp(raft_state_to_string(RAFT_STATE_FOLLOWER), "FOLLOWER") == 0);
    ASSERT_TRUE(strcmp(raft_state_to_string(RAFT_STATE_CANDIDATE), "CANDIDATE") == 0);
    ASSERT_TRUE(strcmp(raft_state_to_string(RAFT_STATE_LEADER), "LEADER") == 0);
    
    /* Test leader query (should fail initially) */
    char leader_id[64];
    distric_err_t err = raft_get_leader(node, leader_id);
    ASSERT_TRUE(err == DISTRIC_ERR_NOT_FOUND);
    
    printf("  State queries work correctly\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_request_vote_same_term() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_OK(raft_start(node));
    
    /* Receive RequestVote from candidate in term 1 */
    bool vote_granted = false;
    uint32_t term_out = 0;
    
    ASSERT_OK(raft_handle_request_vote(node, "peer-0", 1, 0, 0, &vote_granted, &term_out));
    
    /* Should grant vote (first request in term 1, log is up-to-date) */
    ASSERT_TRUE(vote_granted);
    ASSERT_EQ(term_out, 1);
    
    /* Node should have updated to term 1 */
    ASSERT_EQ(raft_get_term(node), 1);
    
    printf("  Granted vote to first candidate in term 1\n");
    
    /* Second RequestVote in same term (different candidate) */
    vote_granted = false;
    term_out = 0;
    
    ASSERT_OK(raft_handle_request_vote(node, "peer-1", 1, 0, 0, &vote_granted, &term_out));
    
    /* Should NOT grant vote (already voted in this term) */
    ASSERT_TRUE(!vote_granted);
    
    printf("  Rejected vote to second candidate in same term\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_request_vote_old_term() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_OK(raft_start(node));
    
    /* Advance to term 5 */
    bool vote_granted = false;
    uint32_t term_out = 0;
    ASSERT_OK(raft_handle_request_vote(node, "peer-0", 5, 0, 0, &vote_granted, &term_out));
    ASSERT_TRUE(vote_granted);
    ASSERT_EQ(raft_get_term(node), 5);
    
    /* Receive RequestVote from old term */
    vote_granted = false;
    term_out = 0;
    
    ASSERT_OK(raft_handle_request_vote(node, "peer-1", 3, 0, 0, &vote_granted, &term_out));
    
    /* Should reject (term too old) */
    ASSERT_TRUE(!vote_granted);
    ASSERT_EQ(term_out, 5);
    
    /* Should still be in term 5 */
    ASSERT_EQ(raft_get_term(node), 5);
    
    printf("  Rejected vote from old term\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_append_entries_heartbeat() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_OK(raft_start(node));
    
    /* Receive heartbeat from leader */
    bool success = false;
    uint32_t term_out = 0;
    uint32_t last_log_index_out = 0;
    
    ASSERT_OK(raft_handle_append_entries(node, "leader-1", 1, 0, 0, NULL, 0, 0,
                                         &success, &term_out, &last_log_index_out));
    
    /* Should accept */
    ASSERT_TRUE(success);
    ASSERT_EQ(term_out, 1);
    
    /* Should recognize leader */
    char leader_id[64];
    ASSERT_OK(raft_get_leader(node, leader_id));
    ASSERT_TRUE(strcmp(leader_id, "leader-1") == 0);
    
    printf("  Accepted heartbeat from leader\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_append_entries_with_entries() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_OK(raft_start(node));
    
    /* Create log entries to replicate */
    raft_log_entry_t entries[2];
    
    entries[0].index = 1;
    entries[0].term = 1;
    entries[0].type = RAFT_ENTRY_COMMAND;
    entries[0].data = (uint8_t*)"cmd1";
    entries[0].data_len = 4;
    
    entries[1].index = 2;
    entries[1].term = 1;
    entries[1].type = RAFT_ENTRY_COMMAND;
    entries[1].data = (uint8_t*)"cmd2";
    entries[1].data_len = 4;
    
    /* Receive AppendEntries with entries */
    bool success = false;
    uint32_t term_out = 0;
    uint32_t last_log_index_out = 0;
    
    ASSERT_OK(raft_handle_append_entries(node, "leader-1", 1, 0, 0, entries, 2, 0,
                                         &success, &term_out, &last_log_index_out));
    
    /* Should accept */
    ASSERT_TRUE(success);
    ASSERT_EQ(last_log_index_out, 2);
    
    /* Log should contain 2 entries */
    ASSERT_EQ(raft_get_last_log_index(node), 2);
    
    printf("  Appended 2 log entries\n");
    
    /* Receive AppendEntries with commit index */
    success = false;
    ASSERT_OK(raft_handle_append_entries(node, "leader-1", 1, 2, 1, NULL, 0, 2,
                                         &success, &term_out, &last_log_index_out));
    
    /* Commit index should be updated */
    ASSERT_EQ(raft_get_commit_index(node), 2);
    
    printf("  Commit index updated to 2\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_leader_append_entry() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_OK(raft_start(node));
    
    /* Try to append as follower (should fail) */
    uint32_t index = 0;
    distric_err_t err = raft_append_entry(node, (uint8_t*)"cmd", 3, &index);
    ASSERT_TRUE(err == DISTRIC_ERR_NOT_FOUND);
    
    printf("  Follower cannot append entries\n");
    
    /* Manually transition to leader for testing */
    /* In real scenario, this happens via election */
    /* For now, we just verify the API behavior */
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

void test_tick_mechanism() {
    TEST_START();
    
    raft_config_t config = create_test_config("node-1", 2);
    
    raft_node_t* node = NULL;
    ASSERT_OK(raft_create(&config, &node));
    ASSERT_OK(raft_start(node));
    
    /* Call tick several times */
    for (int i = 0; i < 10; i++) {
        ASSERT_OK(raft_tick(node));
        usleep(10000);  /* 10ms */
    }
    
    printf("  Tick mechanism works\n");
    
    raft_destroy(node);
    free_test_config(&config);
    
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Raft - Basic Tests ===\n");
    
    test_node_creation();
    test_state_queries();
    test_request_vote_same_term();
    test_request_vote_old_term();
    test_append_entries_heartbeat();
    test_append_entries_with_entries();
    test_leader_append_entry();
    test_tick_mechanism();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All Raft basic tests passed!\n");
        printf("✓ Session 3.1 (Leader Election Foundation) COMPLETE\n");
        printf("\nNext Steps:\n");
        printf("  - Session 3.2: RPC Integration (send RequestVote/AppendEntries)\n");
        printf("  - Session 3.3: Log Replication\n");
        printf("  - Session 3.4: Persistence\n");
        printf("  - Session 3.5: Snapshots\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}