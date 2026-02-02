/**
 * @file test_messages.c
 * @brief Comprehensive tests for protocol message serialization
 * 
 * Tests all message types:
 * - Raft messages (RequestVote, AppendEntries, etc.)
 * - Gossip messages (Ping, Ack, MembershipUpdate)
 * - Task messages (Assignment, Result)
 * - Client messages (Submit, Response)
 */

#include <distric_protocol/messages.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s (%lld) != %s (%lld)\n", #a, (long long)(a), #b, (long long)(b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "FAIL: %s (\"%s\") != %s (\"%s\")\n", #a, (a), #b, (b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* ============================================================================
 * RAFT MESSAGE TESTS
 * ========================================================================= */

void test_raft_request_vote() {
    TEST_START();
    
    /* Create message */
    raft_request_vote_t msg = {
        .term = 42,
        .last_log_index = 1000,
        .last_log_term = 41
    };
    strncpy(msg.candidate_id, "coordinator-1", sizeof(msg.candidate_id) - 1);
    
    /* Serialize */
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_raft_request_vote(&msg, &buffer, &len));
    ASSERT_TRUE(buffer != NULL);
    ASSERT_TRUE(len > 0);
    
    printf("  Serialized size: %zu bytes\n", len);
    
    /* Deserialize */
    raft_request_vote_t decoded;
    ASSERT_OK(deserialize_raft_request_vote(buffer, len, &decoded));
    
    /* Verify */
    ASSERT_EQ(decoded.term, 42);
    ASSERT_STR_EQ(decoded.candidate_id, "coordinator-1");
    ASSERT_EQ(decoded.last_log_index, 1000);
    ASSERT_EQ(decoded.last_log_term, 41);
    
    free(buffer);
    TEST_PASS();
}

void test_raft_request_vote_response() {
    TEST_START();
    
    raft_request_vote_response_t msg = {
        .term = 42,
        .vote_granted = true
    };
    strncpy(msg.node_id, "node-5", sizeof(msg.node_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_raft_request_vote_response(&msg, &buffer, &len));
    
    raft_request_vote_response_t decoded;
    ASSERT_OK(deserialize_raft_request_vote_response(buffer, len, &decoded));
    
    ASSERT_EQ(decoded.term, 42);
    ASSERT_TRUE(decoded.vote_granted);
    ASSERT_STR_EQ(decoded.node_id, "node-5");
    
    free(buffer);
    TEST_PASS();
}

void test_raft_append_entries_empty() {
    TEST_START();
    
    /* Heartbeat (no entries) */
    raft_append_entries_t msg = {
        .term = 42,
        .prev_log_index = 999,
        .prev_log_term = 41,
        .leader_commit = 950,
        .entries = NULL,
        .entry_count = 0
    };
    strncpy(msg.leader_id, "leader-1", sizeof(msg.leader_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_raft_append_entries(&msg, &buffer, &len));
    
    printf("  Heartbeat size: %zu bytes\n", len);
    
    raft_append_entries_t decoded;
    ASSERT_OK(deserialize_raft_append_entries(buffer, len, &decoded));
    
    ASSERT_EQ(decoded.term, 42);
    ASSERT_STR_EQ(decoded.leader_id, "leader-1");
    ASSERT_EQ(decoded.prev_log_index, 999);
    ASSERT_EQ(decoded.entry_count, 0);
    
    free_raft_append_entries(&decoded);
    free(buffer);
    TEST_PASS();
}

void test_raft_append_entries_with_entries() {
    TEST_START();
    
    /* Create log entries */
    raft_log_entry_t entries[3];
    
    entries[0].index = 1000;
    entries[0].term = 42;
    entries[0].data = (uint8_t*)"command1";
    entries[0].data_len = 8;
    
    entries[1].index = 1001;
    entries[1].term = 42;
    entries[1].data = (uint8_t*)"command2";
    entries[1].data_len = 8;
    
    entries[2].index = 1002;
    entries[2].term = 42;
    entries[2].data = (uint8_t*)"command3";
    entries[2].data_len = 8;
    
    raft_append_entries_t msg = {
        .term = 42,
        .prev_log_index = 999,
        .prev_log_term = 41,
        .leader_commit = 950,
        .entries = entries,
        .entry_count = 3
    };
    strncpy(msg.leader_id, "leader-1", sizeof(msg.leader_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_raft_append_entries(&msg, &buffer, &len));
    
    printf("  With 3 entries size: %zu bytes\n", len);
    
    raft_append_entries_t decoded;
    ASSERT_OK(deserialize_raft_append_entries(buffer, len, &decoded));
    
    ASSERT_EQ(decoded.entry_count, 3);
    ASSERT_EQ(decoded.entries[0].index, 1000);
    ASSERT_EQ(decoded.entries[1].index, 1001);
    ASSERT_EQ(decoded.entries[2].index, 1002);
    
    /* Verify data */
    ASSERT_TRUE(memcmp(decoded.entries[0].data, "command1", 8) == 0);
    ASSERT_TRUE(memcmp(decoded.entries[1].data, "command2", 8) == 0);
    ASSERT_TRUE(memcmp(decoded.entries[2].data, "command3", 8) == 0);
    
    free_raft_append_entries(&decoded);
    free(buffer);
    TEST_PASS();
}

/* ============================================================================
 * GOSSIP MESSAGE TESTS
 * ========================================================================= */

void test_gossip_ping() {
    TEST_START();
    
    gossip_ping_t msg = {
        .incarnation = 123456,
        .sequence_number = 789
    };
    strncpy(msg.sender_id, "node-1", sizeof(msg.sender_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_gossip_ping(&msg, &buffer, &len));
    
    gossip_ping_t decoded;
    ASSERT_OK(deserialize_gossip_ping(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.sender_id, "node-1");
    ASSERT_EQ(decoded.incarnation, 123456);
    ASSERT_EQ(decoded.sequence_number, 789);
    
    free(buffer);
    TEST_PASS();
}

void test_gossip_ack() {
    TEST_START();
    
    gossip_ack_t msg = {
        .incarnation = 654321,
        .sequence_number = 789
    };
    strncpy(msg.sender_id, "node-2", sizeof(msg.sender_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_gossip_ack(&msg, &buffer, &len));
    
    gossip_ack_t decoded;
    ASSERT_OK(deserialize_gossip_ack(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.sender_id, "node-2");
    ASSERT_EQ(decoded.incarnation, 654321);
    ASSERT_EQ(decoded.sequence_number, 789);
    
    free(buffer);
    TEST_PASS();
}

void test_gossip_membership_update() {
    TEST_START();
    
    /* Create node updates */
    gossip_node_info_t updates[3];
    
    strncpy(updates[0].node_id, "node-1", sizeof(updates[0].node_id) - 1);
    strncpy(updates[0].address, "10.0.1.1", sizeof(updates[0].address) - 1);
    updates[0].port = 9000;
    updates[0].state = NODE_STATE_ALIVE;
    updates[0].role = NODE_ROLE_COORDINATOR;
    updates[0].incarnation = 100;
    
    strncpy(updates[1].node_id, "node-2", sizeof(updates[1].node_id) - 1);
    strncpy(updates[1].address, "10.0.1.2", sizeof(updates[1].address) - 1);
    updates[1].port = 9000;
    updates[1].state = NODE_STATE_ALIVE;
    updates[1].role = NODE_ROLE_COORDINATOR;
    updates[1].incarnation = 101;
    
    strncpy(updates[2].node_id, "worker-1", sizeof(updates[2].node_id) - 1);
    strncpy(updates[2].address, "10.0.2.1", sizeof(updates[2].address) - 1);
    updates[2].port = 9001;
    updates[2].state = NODE_STATE_SUSPECTED;
    updates[2].role = NODE_ROLE_WORKER;
    updates[2].incarnation = 200;
    
    gossip_membership_update_t msg = {
        .updates = updates,
        .update_count = 3
    };
    strncpy(msg.sender_id, "node-1", sizeof(msg.sender_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_gossip_membership_update(&msg, &buffer, &len));
    
    printf("  3 node updates size: %zu bytes\n", len);
    
    gossip_membership_update_t decoded;
    ASSERT_OK(deserialize_gossip_membership_update(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.sender_id, "node-1");
    ASSERT_EQ(decoded.update_count, 3);
    
    /* Verify first node */
    ASSERT_STR_EQ(decoded.updates[0].node_id, "node-1");
    ASSERT_STR_EQ(decoded.updates[0].address, "10.0.1.1");
    ASSERT_EQ(decoded.updates[0].port, 9000);
    ASSERT_EQ(decoded.updates[0].state, NODE_STATE_ALIVE);
    ASSERT_EQ(decoded.updates[0].role, NODE_ROLE_COORDINATOR);
    
    /* Verify third node (suspected worker) */
    ASSERT_STR_EQ(decoded.updates[2].node_id, "worker-1");
    ASSERT_EQ(decoded.updates[2].state, NODE_STATE_SUSPECTED);
    ASSERT_EQ(decoded.updates[2].role, NODE_ROLE_WORKER);
    
    free_gossip_membership_update(&decoded);
    free(buffer);
    TEST_PASS();
}

/* ============================================================================
 * TASK MESSAGE TESTS
 * ========================================================================= */

void test_task_assignment() {
    TEST_START();
    
    uint8_t input_data[] = {0x01, 0x02, 0x03, 0x04};
    
    task_assignment_t msg = {
        .config_json = "{\"threshold\":1000}",
        .input_data = input_data,
        .input_data_len = 4,
        .timeout_sec = 30,
        .retry_count = 3
    };
    strncpy(msg.task_id, "task-123", sizeof(msg.task_id) - 1);
    strncpy(msg.workflow_id, "workflow-456", sizeof(msg.workflow_id) - 1);
    strncpy(msg.task_type, "payment_validator", sizeof(msg.task_type) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_task_assignment(&msg, &buffer, &len));
    
    printf("  Task assignment size: %zu bytes\n", len);
    
    task_assignment_t decoded;
    ASSERT_OK(deserialize_task_assignment(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.task_id, "task-123");
    ASSERT_STR_EQ(decoded.workflow_id, "workflow-456");
    ASSERT_STR_EQ(decoded.task_type, "payment_validator");
    ASSERT_STR_EQ(decoded.config_json, "{\"threshold\":1000}");
    ASSERT_EQ(decoded.input_data_len, 4);
    ASSERT_TRUE(memcmp(decoded.input_data, input_data, 4) == 0);
    ASSERT_EQ(decoded.timeout_sec, 30);
    ASSERT_EQ(decoded.retry_count, 3);
    
    free_task_assignment(&decoded);
    free(buffer);
    TEST_PASS();
}

void test_task_result() {
    TEST_START();
    
    uint8_t output_data[] = "result_data";
    
    task_result_t msg = {
        .status = TASK_STATUS_COMPLETED,
        .output_data = output_data,
        .output_data_len = 11,
        .error_message = NULL,
        .exit_code = 0,
        .started_at = 1000000,
        .completed_at = 1005000
    };
    strncpy(msg.task_id, "task-123", sizeof(msg.task_id) - 1);
    strncpy(msg.worker_id, "worker-1", sizeof(msg.worker_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_task_result(&msg, &buffer, &len));
    
    printf("  Task result size: %zu bytes\n", len);
    
    task_result_t decoded;
    ASSERT_OK(deserialize_task_result(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.task_id, "task-123");
    ASSERT_STR_EQ(decoded.worker_id, "worker-1");
    ASSERT_EQ(decoded.status, TASK_STATUS_COMPLETED);
    ASSERT_EQ(decoded.output_data_len, 11);
    ASSERT_TRUE(memcmp(decoded.output_data, output_data, 11) == 0);
    ASSERT_EQ(decoded.exit_code, 0);
    ASSERT_EQ(decoded.started_at, 1000000);
    ASSERT_EQ(decoded.completed_at, 1005000);
    
    free_task_result(&decoded);
    free(buffer);
    TEST_PASS();
}

/* ============================================================================
 * CLIENT MESSAGE TESTS
 * ========================================================================= */

void test_client_submit() {
    TEST_START();
    
    client_submit_t msg = {
        .payload_json = "{\"amount\":15000,\"user_id\":\"user_123\"}",
        .timestamp = 1700000000000ULL
    };
    strncpy(msg.message_id, "msg-abc123", sizeof(msg.message_id) - 1);
    strncpy(msg.event_type, "payment_received", sizeof(msg.event_type) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_client_submit(&msg, &buffer, &len));
    
    printf("  Client submit size: %zu bytes\n", len);
    
    client_submit_t decoded;
    ASSERT_OK(deserialize_client_submit(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.message_id, "msg-abc123");
    ASSERT_STR_EQ(decoded.event_type, "payment_received");
    ASSERT_STR_EQ(decoded.payload_json, "{\"amount\":15000,\"user_id\":\"user_123\"}");
    ASSERT_EQ(decoded.timestamp, 1700000000000ULL);
    
    free_client_submit(&decoded);
    free(buffer);
    TEST_PASS();
}

void test_client_response() {
    TEST_START();
    
    char* workflows[] = {"fraud_detection", "notification", "analytics"};
    
    client_response_t msg = {
        .response_code = 202,
        .response_message = "Accepted for processing",
        .workflows_triggered = workflows,
        .workflow_count = 3
    };
    strncpy(msg.message_id, "msg-abc123", sizeof(msg.message_id) - 1);
    
    uint8_t* buffer = NULL;
    size_t len = 0;
    ASSERT_OK(serialize_client_response(&msg, &buffer, &len));
    
    printf("  Client response size: %zu bytes\n", len);
    
    client_response_t decoded;
    ASSERT_OK(deserialize_client_response(buffer, len, &decoded));
    
    ASSERT_STR_EQ(decoded.message_id, "msg-abc123");
    ASSERT_EQ(decoded.response_code, 202);
    ASSERT_STR_EQ(decoded.response_message, "Accepted for processing");
    ASSERT_EQ(decoded.workflow_count, 3);
    ASSERT_STR_EQ(decoded.workflows_triggered[0], "fraud_detection");
    ASSERT_STR_EQ(decoded.workflows_triggered[1], "notification");
    ASSERT_STR_EQ(decoded.workflows_triggered[2], "analytics");
    
    free_client_response(&decoded);
    free(buffer);
    TEST_PASS();
}

/* ============================================================================
 * UTILITY FUNCTION TESTS
 * ========================================================================= */

void test_utility_functions() {
    TEST_START();
    
    ASSERT_STR_EQ(node_state_to_string(NODE_STATE_ALIVE), "ALIVE");
    ASSERT_STR_EQ(node_state_to_string(NODE_STATE_SUSPECTED), "SUSPECTED");
    ASSERT_STR_EQ(node_state_to_string(NODE_STATE_FAILED), "FAILED");
    
    ASSERT_STR_EQ(node_role_to_string(NODE_ROLE_COORDINATOR), "COORDINATOR");
    ASSERT_STR_EQ(node_role_to_string(NODE_ROLE_WORKER), "WORKER");
    
    ASSERT_STR_EQ(task_status_to_string(TASK_STATUS_PENDING), "PENDING");
    ASSERT_STR_EQ(task_status_to_string(TASK_STATUS_RUNNING), "RUNNING");
    ASSERT_STR_EQ(task_status_to_string(TASK_STATUS_COMPLETED), "COMPLETED");
    ASSERT_STR_EQ(task_status_to_string(TASK_STATUS_FAILED), "FAILED");
    
    printf("  All utility functions work correctly\n");
    
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Protocol - Message Serialization Tests ===\n");
    
    /* Raft messages */
    test_raft_request_vote();
    test_raft_request_vote_response();
    test_raft_append_entries_empty();
    test_raft_append_entries_with_entries();
    
    /* Gossip messages */
    test_gossip_ping();
    test_gossip_ack();
    test_gossip_membership_update();
    
    /* Task messages */
    test_task_assignment();
    test_task_result();
    
    /* Client messages */
    test_client_submit();
    test_client_response();
    
    /* Utilities */
    test_utility_functions();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All message serialization tests passed!\n");
        printf("✓ Session 2.3 COMPLETE - Ready for Session 2.4 (RPC Framework)\n");
        printf("\nMessage Types Tested:\n");
        printf("  - Raft: RequestVote, AppendEntries (with/without entries)\n");
        printf("  - Gossip: Ping, Ack, MembershipUpdate\n");
        printf("  - Task: Assignment, Result\n");
        printf("  - Client: Submit, Response\n");
        printf("\nAll messages use TLV encoding for forward compatibility!\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}