/**
 * @file test_protocol_binary.c
 * @brief Comprehensive tests for the binary protocol header
 *
 * Updated for improvements applied to distric_protocol:
 *
 *  Improvement #5 — reserved field no longer rejected by validate_message_header.
 *    The old test expected !validate_message_header() when reserved != 0.
 *    The new test asserts the OPPOSITE: non-zero reserved values MUST be
 *    accepted (forward-compatibility requirement).
 *
 *  Improvement #6 — timestamp field renamed from timestamp_us to timestamp_s.
 *    All references to header.timestamp_us updated to header.timestamp_s.
 *    The field now stores Unix epoch seconds; the test verifies it is non-zero.
 *
 *  Improvement #10 — added test_crc32_known_vector() which verifies the
 *    compile-time CRC32 table against the well-known test vector
 *    CRC32("123456789") == 0xCBF43926 (RFC 1952).
 */

#include <distric_protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START() printf("\n[TEST] %s...\n", __func__)
#define TEST_PASS()  do { printf("[PASS] %s\n", __func__); tests_passed++; } while(0)

#define ASSERT_OK(expr) \
    do { \
        distric_err_t _err = (expr); \
        if (_err != DISTRIC_OK) { \
            fprintf(stderr, "  FAIL: %s returned %d  (%s:%d)\n", \
                    #expr, _err, __FILE__, __LINE__); \
            tests_failed++; return; \
        } \
    } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "  FAIL: %s is false  (%s:%d)\n", \
                    #expr, __FILE__, __LINE__); \
            tests_failed++; return; \
        } \
    } while(0)

/* ============================================================================
 * TEST: Header size must be exactly 32 bytes
 * ========================================================================= */

void test_header_size(void)
{
    TEST_START();
    ASSERT_TRUE(sizeof(message_header_t) == MESSAGE_HEADER_SIZE);
    ASSERT_TRUE(MESSAGE_HEADER_SIZE == 32);
    printf("  Header size: %zu bytes (expected 32)\n",
           sizeof(message_header_t));
    TEST_PASS();
}

/* ============================================================================
 * TEST: Header initialisation
 * ========================================================================= */

void test_header_initialization(void)
{
    TEST_START();

    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_RAFT_REQUEST_VOTE, 1024));

    ASSERT_TRUE(header.magic       == PROTOCOL_MAGIC);
    ASSERT_TRUE(header.version     == PROTOCOL_VERSION);
    ASSERT_TRUE(header.msg_type    == MSG_RAFT_REQUEST_VOTE);
    ASSERT_TRUE(header.flags       == MSG_FLAG_NONE);
    ASSERT_TRUE(header.reserved    == 0);
    ASSERT_TRUE(header.payload_len == 1024);
    ASSERT_TRUE(header.message_id  != 0);

    /*
     * Improvement #6: field is now timestamp_s (Unix epoch seconds).
     * It must be non-zero for any recent wall-clock time.
     */
    ASSERT_TRUE(header.timestamp_s != 0);

    printf("  Magic:       0x%08X\n", header.magic);
    printf("  Version:     0x%04X\n", header.version);
    printf("  Message ID:  %llu\n",   (unsigned long long)header.message_id);
    printf("  Timestamp_s: %u\n",     header.timestamp_s);

    TEST_PASS();
}

/* ============================================================================
 * TEST: 1 000 unique message IDs
 * ========================================================================= */

void test_unique_message_ids(void)
{
    TEST_START();

    message_header_t headers[1000];
    for (int i = 0; i < 1000; i++)
        ASSERT_OK(message_header_init(&headers[i], MSG_CLIENT_SUBMIT, 0));

    for (int i = 0; i < 999; i++)
        for (int j = i + 1; j < 1000; j++)
            ASSERT_TRUE(headers[i].message_id != headers[j].message_id);

    printf("  1 000 unique message IDs generated\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: Serialisation / deserialisation round-trip
 * ========================================================================= */

void test_serialization_deserialization(void)
{
    TEST_START();

    message_header_t original;
    ASSERT_OK(message_header_init(&original, MSG_GOSSIP_PING, 512));
    original.flags = MSG_FLAG_URGENT | MSG_FLAG_COMPRESSED;

    uint8_t buffer[MESSAGE_HEADER_SIZE];
    ASSERT_OK(serialize_header(&original, buffer));

    message_header_t deserialized;
    ASSERT_OK(deserialize_header(buffer, &deserialized));

    ASSERT_TRUE(deserialized.magic       == original.magic);
    ASSERT_TRUE(deserialized.version     == original.version);
    ASSERT_TRUE(deserialized.msg_type    == original.msg_type);
    ASSERT_TRUE(deserialized.flags       == original.flags);
    ASSERT_TRUE(deserialized.reserved    == original.reserved);
    ASSERT_TRUE(deserialized.payload_len == original.payload_len);
    ASSERT_TRUE(deserialized.message_id  == original.message_id);
    /* Improvement #6: field renamed to timestamp_s */
    ASSERT_TRUE(deserialized.timestamp_s == original.timestamp_s);
    ASSERT_TRUE(deserialized.crc32       == original.crc32);

    printf("  Round-trip serialisation successful\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: Network byte order
 * ========================================================================= */

void test_network_byte_order(void)
{
    TEST_START();

    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_TASK_ASSIGNMENT, 0x12345678));
    header.message_id = 0x0102030405060708ULL;

    uint8_t buffer[MESSAGE_HEADER_SIZE];
    ASSERT_OK(serialize_header(&header, buffer));

    /* Magic 0xD15C0000 at offset 0 — big-endian */
    ASSERT_TRUE(buffer[0] == 0xD1);
    ASSERT_TRUE(buffer[1] == 0x5C);
    ASSERT_TRUE(buffer[2] == 0x00);
    ASSERT_TRUE(buffer[3] == 0x00);

    /* Payload length at offset 12 — big-endian */
    uint32_t payload_be = ((uint32_t)buffer[12] << 24) |
                          ((uint32_t)buffer[13] << 16) |
                          ((uint32_t)buffer[14] <<  8) |
                          ((uint32_t)buffer[15]);
    ASSERT_TRUE(payload_be == 0x12345678);

    /* Message ID at offset 16 — big-endian */
    ASSERT_TRUE(buffer[16] == 0x01);
    ASSERT_TRUE(buffer[17] == 0x02);
    ASSERT_TRUE(buffer[18] == 0x03);

    printf("  Network byte order verified\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: Header validation
 *
 * Improvement #5: a non-zero reserved field MUST now be ACCEPTED.
 * ========================================================================= */

void test_header_validation(void)
{
    TEST_START();

    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_CLIENT_QUERY, 100));
    ASSERT_TRUE(validate_message_header(&header));

    /* Invalid magic */
    header.magic = 0xDEADBEEF;
    ASSERT_TRUE(!validate_message_header(&header));
    header.magic = PROTOCOL_MAGIC;

    /* Invalid major version */
    header.version = 0x9900;
    ASSERT_TRUE(!validate_message_header(&header));
    header.version = PROTOCOL_VERSION;

    /* Invalid message type (0) */
    header.msg_type = 0;
    ASSERT_TRUE(!validate_message_header(&header));
    header.msg_type = MSG_CLIENT_QUERY;

    /* Payload too large */
    header.payload_len = PROTOCOL_MAX_PAYLOAD_SIZE + 1;
    ASSERT_TRUE(!validate_message_header(&header));
    header.payload_len = 100;

    /* Payload exactly at the limit — must be accepted */
    header.payload_len = PROTOCOL_MAX_PAYLOAD_SIZE;
    ASSERT_TRUE(validate_message_header(&header));
    header.payload_len = 100;

    /*
     * Improvement #5: non-zero reserved field MUST be accepted for
     * forward-compatibility with future minor versions.
     */
    header.reserved = 1;
    ASSERT_TRUE(validate_message_header(&header));   /* was !validate in old code */
    header.reserved = 0xFFFF;
    ASSERT_TRUE(validate_message_header(&header));
    header.reserved = 0;

    /* Still valid after reset */
    ASSERT_TRUE(validate_message_header(&header));

    printf("  Validation checks passed (reserved field correctly tolerated)\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: CRC32 computation
 * ========================================================================= */

void test_crc32_computation(void)
{
    TEST_START();

    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_RAFT_APPEND_ENTRIES, 256));

    uint8_t payload[256];
    for (int i = 0; i < 256; i++) payload[i] = (uint8_t)i;

    ASSERT_OK(compute_header_crc32(&header, payload, 256));
    ASSERT_TRUE(header.crc32 != 0);
    ASSERT_TRUE(verify_message_crc32(&header, payload, 256));

    printf("  CRC32: 0x%08X — computation and verification OK\n", header.crc32);
    TEST_PASS();
}

/* ============================================================================
 * TEST: CRC32 detects single-bit corruption
 * ========================================================================= */

void test_crc32_detects_corruption(void)
{
    TEST_START();

    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_GOSSIP_MEMBERSHIP_UPDATE, 128));

    uint8_t payload[128];
    memset(payload, 0xAA, 128);

    ASSERT_OK(compute_header_crc32(&header, payload, 128));
    ASSERT_TRUE(verify_message_crc32(&header, payload, 128));

    /* Flip one payload bit */
    payload[50] ^= 0x01;
    ASSERT_TRUE(!verify_message_crc32(&header, payload, 128));
    payload[50] ^= 0x01;

    /* Flip one header field bit */
    uint32_t saved_crc = header.crc32;
    header.msg_type ^= 0x0001;
    ASSERT_TRUE(!verify_message_crc32(&header, payload, 128));
    header.msg_type ^= 0x0001;
    header.crc32 = saved_crc;

    ASSERT_TRUE(verify_message_crc32(&header, payload, 128));
    printf("  Single-bit corruption detected in both header and payload\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: Empty-payload CRC
 * ========================================================================= */

void test_empty_payload_crc(void)
{
    TEST_START();

    message_header_t header;
    ASSERT_OK(message_header_init(&header, MSG_GOSSIP_ACK, 0));
    ASSERT_OK(compute_header_crc32(&header, NULL, 0));
    ASSERT_TRUE(header.crc32 != 0);
    ASSERT_TRUE(verify_message_crc32(&header, NULL, 0));

    printf("  Empty-payload CRC works\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: CRC32 known test vector (Improvement #10 verification)
 *
 * CRC32 of the ASCII string "123456789" must equal 0xCBF43926 per RFC 1952.
 * ========================================================================= */

void test_crc32_known_vector(void)
{
    TEST_START();

    const uint8_t data[] = "123456789";
    uint32_t crc = compute_crc32(data, 9);   /* 9 bytes, no null terminator */

    printf("  CRC32(\"123456789\") = 0x%08X (expected 0xCBF43926)\n", crc);
    ASSERT_TRUE(crc == 0xCBF43926u);

    TEST_PASS();
}

/* ============================================================================
 * TEST: message_type_to_string
 * ========================================================================= */

void test_message_type_strings(void)
{
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

    printf("  All message type strings correct\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: All message types round-trip
 * ========================================================================= */

void test_all_message_types(void)
{
    TEST_START();

    const message_type_t types[] = {
        MSG_RAFT_REQUEST_VOTE, MSG_RAFT_APPEND_ENTRIES,
        MSG_GOSSIP_PING, MSG_GOSSIP_MEMBERSHIP_UPDATE,
        MSG_TASK_ASSIGNMENT, MSG_TASK_RESULT,
        MSG_CLIENT_SUBMIT, MSG_CLIENT_RESPONSE,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        message_header_t header;
        ASSERT_OK(message_header_init(&header, types[i], 64));
        ASSERT_TRUE(validate_message_header(&header));
        ASSERT_OK(compute_header_crc32(&header, NULL, 0));
        ASSERT_TRUE(verify_message_crc32(&header, NULL, 0));
    }

    printf("  All message types serialise and validate correctly\n");
    TEST_PASS();
}

/* ============================================================================
 * TEST: Portability (sender → wire → receiver)
 * ========================================================================= */

void test_portability(void)
{
    TEST_START();

    message_header_t sender;
    ASSERT_OK(message_header_init(&sender, MSG_RAFT_REQUEST_VOTE, 1024));

    uint8_t payload[1024];
    memset(payload, 0x42, 1024);
    ASSERT_OK(compute_header_crc32(&sender, payload, 1024));

    uint8_t wire[MESSAGE_HEADER_SIZE];
    ASSERT_OK(serialize_header(&sender, wire));

    message_header_t receiver;
    ASSERT_OK(deserialize_header(wire, &receiver));

    ASSERT_TRUE(receiver.magic       == sender.magic);
    ASSERT_TRUE(receiver.version     == sender.version);
    ASSERT_TRUE(receiver.msg_type    == sender.msg_type);
    ASSERT_TRUE(receiver.payload_len == sender.payload_len);
    ASSERT_TRUE(receiver.message_id  == sender.message_id);
    ASSERT_TRUE(receiver.crc32       == sender.crc32);
    /* Improvement #6 */
    ASSERT_TRUE(receiver.timestamp_s == sender.timestamp_s);

    ASSERT_TRUE(verify_message_crc32(&receiver, payload, 1024));

    printf("  Portability test passed (sender → wire → receiver)\n");
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void)
{
    printf("=== DistriC Protocol — Binary Header Tests ===\n");

    test_header_size();
    test_header_initialization();
    test_unique_message_ids();
    test_serialization_deserialization();
    test_network_byte_order();
    test_header_validation();
    test_crc32_computation();
    test_crc32_detects_corruption();
    test_empty_payload_crc();
    test_crc32_known_vector();      /* Improvement #10 */
    test_message_type_strings();
    test_all_message_types();
    test_portability();

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\n✓ All binary protocol tests passed!\n");
    }

    return tests_failed > 0 ? 1 : 0;
}