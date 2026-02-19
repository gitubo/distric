/**
 * @file test_binary.c
 * @brief Comprehensive tests for binary protocol header
 * 
 * Tests:
 * - Header initialization
 * - Serialization/deserialization
 * - Network byte order conversion
 * - CRC32 computation and verification
 * - Message validation
 * - Corruption detection
 */

#include <distric_protocol.h>
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

/* ============================================================================
 * TEST CASES
 * ========================================================================= */

void test_header_initialization() {
    TEST_START();
    
    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_RAFT_REQUEST_VOTE, 1024));
    
    ASSERT_TRUE(header.magic == PROTOCOL_MAGIC);
    ASSERT_TRUE(header.version == PROTOCOL_VERSION);
    ASSERT_TRUE(header.msg_type == MSG_RAFT_REQUEST_VOTE);
    ASSERT_TRUE(header.flags == MSG_FLAG_NONE);
    ASSERT_TRUE(header.payload_len == 1024);
    ASSERT_TRUE(header.message_id != 0);
    ASSERT_TRUE(header.timestamp_us != 0);
    ASSERT_TRUE(header.reserved == 0);
    
    printf("  Magic: 0x%08X\n", header.magic);
    printf("  Version: 0x%04X\n", header.version);
    printf("  Message ID: %lu\n", header.message_id);
    printf("  Timestamp (us): %u\n", header.timestamp_us);
    
    TEST_PASS();
}

void test_unique_message_ids() {
    TEST_START();
    
    message_header_t headers[1000];
    
    /* Generate 1000 message IDs */
    for (int i = 0; i < 1000; i++) {
        ASSERT_OK(message_header_init(&headers[i], MSG_CLIENT_SUBMIT, 0));
    }
    
    /* Check all are unique */
    for (int i = 0; i < 999; i++) {
        for (int j = i + 1; j < 1000; j++) {
            ASSERT_TRUE(headers[i].message_id != headers[j].message_id);
        }
    }
    
    printf("  1000 unique message IDs generated\n");
    TEST_PASS();
}

void test_serialization_deserialization() {
    TEST_START();
    
    message_header_t original;
    ASSERT_OK(message_header_init(&original, MSG_GOSSIP_PING, 512));
    original.flags = MSG_FLAG_URGENT | MSG_FLAG_COMPRESSED;
    
    /* Serialize */
    uint8_t buffer[MESSAGE_HEADER_SIZE];
    ASSERT_OK(serialize_header(&original, buffer));
    
    /* Deserialize */
    message_header_t deserialized;
    ASSERT_OK(deserialize_header(buffer, &deserialized));
    
    /* Verify all fields match */
    ASSERT_TRUE(deserialized.magic == original.magic);
    ASSERT_TRUE(deserialized.version == original.version);
    ASSERT_TRUE(deserialized.msg_type == original.msg_type);
    ASSERT_TRUE(deserialized.flags == original.flags);
    ASSERT_TRUE(deserialized.payload_len == original.payload_len);
    ASSERT_TRUE(deserialized.message_id == original.message_id);
    ASSERT_TRUE(deserialized.timestamp_us == original.timestamp_us);
    ASSERT_TRUE(deserialized.crc32 == original.crc32);
    
    printf("  Round-trip serialization successful\n");
    TEST_PASS();
}

void test_network_byte_order() {
    TEST_START();
    
    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_TASK_ASSIGNMENT, 0x12345678));
    header.message_id = 0x0102030405060708ULL;
    
    /* Serialize */
    uint8_t buffer[MESSAGE_HEADER_SIZE];
    ASSERT_OK(serialize_header(&header, buffer));
    
    /* Check network byte order (big-endian) */
    /* Magic at offset 0 should be 0x44 0x49 0x53 0x54 ("DIST") */
    ASSERT_TRUE(buffer[0] == 0x44);
    ASSERT_TRUE(buffer[1] == 0x49);
    ASSERT_TRUE(buffer[2] == 0x53);
    ASSERT_TRUE(buffer[3] == 0x54);
    
    /* Payload length at offset 12 should be big-endian */
    uint32_t payload_be = (buffer[12] << 24) | (buffer[13] << 16) | 
                          (buffer[14] << 8) | buffer[15];
    ASSERT_TRUE(payload_be == 0x12345678);
    
    printf("  Network byte order verified\n");
    TEST_PASS();
}

void test_header_validation() {
    TEST_START();
    
    message_header_t header;
    
    /* Valid header */
    ASSERT_OK(message_header_init(&header, MSG_CLIENT_QUERY, 100));
    ASSERT_TRUE(validate_message_header(&header));
    
    /* Invalid magic */
    header.magic = 0xDEADBEEF;
    ASSERT_TRUE(!validate_message_header(&header));
    header.magic = PROTOCOL_MAGIC;
    
    /* Invalid version */
    header.version = 0x9999;
    ASSERT_TRUE(!validate_message_header(&header));
    header.version = PROTOCOL_VERSION;
    
    /* Invalid message type */
    header.msg_type = 0;
    ASSERT_TRUE(!validate_message_header(&header));
    header.msg_type = MSG_CLIENT_QUERY;
    
    /* Payload too large */
    header.payload_len = 20 * 1024 * 1024;  /* 20MB */
    ASSERT_TRUE(!validate_message_header(&header));
    header.payload_len = 100;
    
    /* Reserved field non-zero */
    header.reserved = 1;
    ASSERT_TRUE(!validate_message_header(&header));
    header.reserved = 0;
    
    /* Now valid again */
    ASSERT_TRUE(validate_message_header(&header));
    
    printf("  Validation checks passed\n");
    TEST_PASS();
}

void test_crc32_computation() {
    TEST_START();
    
    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_RAFT_APPEND_ENTRIES, 256));
    
    /* Create test payload */
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) {
        payload[i] = (uint8_t)i;
    }
    
    /* Compute CRC */
    ASSERT_OK(compute_header_crc32(&header, payload, 256));
    ASSERT_TRUE(header.crc32 != 0);
    
    printf("  CRC32: 0x%08X\n", header.crc32);
    
    /* Verify CRC */
    ASSERT_TRUE(verify_message_crc32(&header, payload, 256));
    
    printf("  CRC32 computation and verification successful\n");
    TEST_PASS();
}

void test_crc32_detects_corruption() {
    TEST_START();
    
    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_GOSSIP_MEMBERSHIP_UPDATE, 128));
    
    uint8_t payload[128];
    memset(payload, 0xAA, 128);
    
    /* Compute valid CRC */
    ASSERT_OK(compute_header_crc32(&header, payload, 128));
    uint32_t original_crc = header.crc32;
    
    /* Verify valid */
    ASSERT_TRUE(verify_message_crc32(&header, payload, 128));
    
    /* Corrupt single bit in payload */
    payload[50] ^= 0x01;
    
    /* CRC should fail */
    ASSERT_TRUE(!verify_message_crc32(&header, payload, 128));
    
    /* Restore payload */
    payload[50] ^= 0x01;
    
    /* Corrupt header field */
    header.msg_type ^= 0x0001;
    
    /* CRC should fail */
    ASSERT_TRUE(!verify_message_crc32(&header, payload, 128));
    
    /* Restore */
    header.msg_type ^= 0x0001;
    header.crc32 = original_crc;
    
    /* Should be valid again */
    ASSERT_TRUE(verify_message_crc32(&header, payload, 128));
    
    printf("  CRC32 detects single-bit corruption\n");
    TEST_PASS();
}

void test_empty_payload_crc() {
    TEST_START();
    
    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_GOSSIP_ACK, 0));
    
    /* Compute CRC with no payload */
    ASSERT_OK(compute_header_crc32(&header, NULL, 0));
    ASSERT_TRUE(header.crc32 != 0);
    
    /* Verify */
    ASSERT_TRUE(verify_message_crc32(&header, NULL, 0));
    
    printf("  Empty payload CRC works\n");
    TEST_PASS();
}

void test_message_type_strings() {
    TEST_START();
    
    ASSERT_TRUE(strcmp(message_type_to_string(MSG_RAFT_REQUEST_VOTE), 
                      "RAFT_REQUEST_VOTE") == 0);
    ASSERT_TRUE(strcmp(message_type_to_string(MSG_GOSSIP_PING), 
                      "GOSSIP_PING") == 0);
    ASSERT_TRUE(strcmp(message_type_to_string(MSG_TASK_ASSIGNMENT), 
                      "TASK_ASSIGNMENT") == 0);
    ASSERT_TRUE(strcmp(message_type_to_string(MSG_CLIENT_SUBMIT), 
                      "CLIENT_SUBMIT") == 0);
    ASSERT_TRUE(strcmp(message_type_to_string((message_type_t)0xFFFF), 
                      "UNKNOWN") == 0);
    
    printf("  Message type strings correct\n");
    TEST_PASS();
}

void test_header_size() {
    TEST_START();
    
    /* Verify header is exactly 32 bytes */
    ASSERT_TRUE(sizeof(message_header_t) == MESSAGE_HEADER_SIZE);
    ASSERT_TRUE(MESSAGE_HEADER_SIZE == 32);
    
    printf("  Header size: %zu bytes (expected: 32)\n", 
           sizeof(message_header_t));
    TEST_PASS();
}

void test_all_message_types() {
    TEST_START();
    
    message_type_t types[] = {
        MSG_RAFT_REQUEST_VOTE,
        MSG_RAFT_APPEND_ENTRIES,
        MSG_GOSSIP_PING,
        MSG_GOSSIP_MEMBERSHIP_UPDATE,
        MSG_TASK_ASSIGNMENT,
        MSG_TASK_RESULT,
        MSG_CLIENT_SUBMIT,
        MSG_CLIENT_RESPONSE
    };
    
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        message_header_t header;
        ASSERT_OK(message_header_init(&header, types[i], 64));
        ASSERT_TRUE(validate_message_header(&header));
        ASSERT_OK(compute_header_crc32(&header, NULL, 0));
        ASSERT_TRUE(verify_message_crc32(&header, NULL, 0));
    }
    
    printf("  All message types work correctly\n");
    TEST_PASS();
}

void test_portability() {
    TEST_START();
    
    /* Create header on "sender" */
    message_header_t sender_header;
    ASSERT_OK(message_header_init(&sender_header, MSG_RAFT_REQUEST_VOTE, 1024));
    
    uint8_t payload[1024];
    memset(payload, 0x42, 1024);
    
    ASSERT_OK(compute_header_crc32(&sender_header, payload, 1024));
    
    /* Serialize */
    uint8_t wire_buffer[MESSAGE_HEADER_SIZE];
    ASSERT_OK(serialize_header(&sender_header, wire_buffer));
    
    /* Simulate transmission... */
    
    /* Deserialize on "receiver" */
    message_header_t receiver_header;
    ASSERT_OK(deserialize_header(wire_buffer, &receiver_header));
    
    /* Verify all fields survived */
    ASSERT_TRUE(receiver_header.magic == sender_header.magic);
    ASSERT_TRUE(receiver_header.version == sender_header.version);
    ASSERT_TRUE(receiver_header.msg_type == sender_header.msg_type);
    ASSERT_TRUE(receiver_header.payload_len == sender_header.payload_len);
    ASSERT_TRUE(receiver_header.message_id == sender_header.message_id);
    ASSERT_TRUE(receiver_header.crc32 == sender_header.crc32);
    
    /* Verify CRC */
    ASSERT_TRUE(verify_message_crc32(&receiver_header, payload, 1024));
    
    printf("  Portability test passed (sender → wire → receiver)\n");
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Protocol - Binary Header Tests ===\n");
    
    test_header_size();
    test_header_initialization();
    test_unique_message_ids();
    test_serialization_deserialization();
    test_network_byte_order();
    test_header_validation();
    test_crc32_computation();
    test_crc32_detects_corruption();
    test_empty_payload_crc();
    test_message_type_strings();
    test_all_message_types();
    test_portability();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All binary protocol tests passed!\n");
        printf("✓ Session 2.1 COMPLETE - Ready for Session 2.2 (TLV)\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}