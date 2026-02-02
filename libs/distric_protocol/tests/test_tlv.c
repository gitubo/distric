/**
 * @file test_tlv.c
 * @brief Comprehensive tests for TLV encoder/decoder
 * 
 * Tests:
 * - Basic encoding/decoding of all types
 * - Round-trip verification
 * - Buffer growth
 * - Zero-copy decoding
 * - Forward compatibility (unknown field skipping)
 * - Corruption detection
 * - Edge cases
 */

#include <distric_protocol/tlv.h>
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

/* Test field tags */
#define TAG_TERM            0x0001
#define TAG_CANDIDATE_ID    0x0002
#define TAG_NODE_ID         0x0003
#define TAG_COUNT           0x0004
#define TAG_TIMESTAMP       0x0005
#define TAG_ENABLED         0x0006
#define TAG_MESSAGE         0x0007
#define TAG_DATA            0x0008

/* ============================================================================
 * BASIC ENCODING/DECODING TESTS
 * ========================================================================= */

void test_encode_decode_uint32() {
    TEST_START();
    
    /* Encode */
    tlv_encoder_t* enc = tlv_encoder_create(64);
    ASSERT_TRUE(enc != NULL);
    
    ASSERT_OK(tlv_encode_uint32(enc, TAG_TERM, 42));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    ASSERT_TRUE(buffer != NULL);
    ASSERT_TRUE(len > 0);
    
    printf("  Encoded length: %zu bytes\n", len);
    
    /* Decode */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    ASSERT_TRUE(dec != NULL);
    
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    ASSERT_EQ(field.type, TLV_UINT32);
    ASSERT_EQ(field.tag, TAG_TERM);
    ASSERT_EQ(field.length, sizeof(uint32_t));
    
    uint32_t value;
    ASSERT_OK(tlv_field_get_uint32(&field, &value));
    ASSERT_EQ(value, 42);
    
    printf("  Decoded value: %u\n", value);
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_encode_decode_string() {
    TEST_START();
    
    const char* test_string = "node-12345";
    
    /* Encode */
    tlv_encoder_t* enc = tlv_encoder_create(64);
    ASSERT_OK(tlv_encode_string(enc, TAG_NODE_ID, test_string));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    printf("  Encoded length: %zu bytes\n", len);
    
    /* Decode */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    ASSERT_EQ(field.type, TLV_STRING);
    ASSERT_EQ(field.tag, TAG_NODE_ID);
    
    const char* decoded = tlv_field_get_string(&field);
    ASSERT_TRUE(decoded != NULL);
    ASSERT_TRUE(strcmp(decoded, test_string) == 0);
    
    printf("  Decoded string: \"%s\"\n", decoded);
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_encode_decode_bytes() {
    TEST_START();
    
    uint8_t test_data[256];
    for (int i = 0; i < 256; i++) {
        test_data[i] = (uint8_t)i;
    }
    
    /* Encode */
    tlv_encoder_t* enc = tlv_encoder_create(512);
    ASSERT_OK(tlv_encode_bytes(enc, TAG_DATA, test_data, 256));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    /* Decode */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    ASSERT_EQ(field.type, TLV_BYTES);
    ASSERT_EQ(field.tag, TAG_DATA);
    ASSERT_EQ(field.length, 256);
    
    size_t decoded_len;
    const uint8_t* decoded = tlv_field_get_bytes(&field, &decoded_len);
    ASSERT_TRUE(decoded != NULL);
    ASSERT_EQ(decoded_len, 256);
    
    /* Verify data */
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(decoded[i], (uint8_t)i);
    }
    
    printf("  256 bytes verified\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_encode_all_numeric_types() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    
    ASSERT_OK(tlv_encode_uint8(enc, 0x0001, 255));
    ASSERT_OK(tlv_encode_uint16(enc, 0x0002, 65535));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0003, 4294967295U));
    ASSERT_OK(tlv_encode_uint64(enc, 0x0004, 18446744073709551615ULL));
    ASSERT_OK(tlv_encode_int32(enc, 0x0005, -2147483648));
    ASSERT_OK(tlv_encode_int64(enc, 0x0006, -9223372036854775807LL));
    ASSERT_OK(tlv_encode_bool(enc, 0x0007, true));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    printf("  Encoded 7 fields, total size: %zu bytes\n", len);
    
    /* Decode and verify */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    /* uint8 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    uint8_t u8;
    ASSERT_OK(tlv_field_get_uint8(&field, &u8));
    ASSERT_EQ(u8, 255);
    
    /* uint16 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    uint16_t u16;
    ASSERT_OK(tlv_field_get_uint16(&field, &u16));
    ASSERT_EQ(u16, 65535);
    
    /* uint32 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    uint32_t u32;
    ASSERT_OK(tlv_field_get_uint32(&field, &u32));
    ASSERT_EQ(u32, 4294967295U);
    
    /* uint64 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    uint64_t u64;
    ASSERT_OK(tlv_field_get_uint64(&field, &u64));
    ASSERT_EQ(u64, 18446744073709551615ULL);
    
    /* int32 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    int32_t i32;
    ASSERT_OK(tlv_field_get_int32(&field, &i32));
    ASSERT_EQ(i32, -2147483648);
    
    /* int64 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    int64_t i64;
    ASSERT_OK(tlv_field_get_int64(&field, &i64));
    ASSERT_EQ(i64, -9223372036854775807LL);
    
    /* bool */
    ASSERT_OK(tlv_decode_next(dec, &field));
    bool b;
    ASSERT_OK(tlv_field_get_bool(&field, &b));
    ASSERT_TRUE(b == true);
    
    printf("  All numeric types verified\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * MULTIPLE FIELDS TEST
 * ========================================================================= */

void test_multiple_fields() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    
    /* Encode multiple fields (simulating Raft RequestVote) */
    ASSERT_OK(tlv_encode_uint32(enc, TAG_TERM, 42));
    ASSERT_OK(tlv_encode_string(enc, TAG_CANDIDATE_ID, "coordinator-1"));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0010, 1000));  /* last_log_index */
    ASSERT_OK(tlv_encode_uint32(enc, 0x0011, 41));    /* last_log_term */
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    printf("  Encoded 4 fields, total size: %zu bytes\n", len);
    
    /* Decode all fields */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    int field_count = 0;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        printf("  Field %d: tag=0x%04X type=%s length=%u\n",
               field_count, field.tag, tlv_type_to_string(field.type), field.length);
        field_count++;
    }
    
    ASSERT_EQ(field_count, 4);
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * BUFFER GROWTH TEST
 * ========================================================================= */

void test_buffer_growth() {
    TEST_START();
    
    /* Start with tiny buffer */
    tlv_encoder_t* enc = tlv_encoder_create(8);
    
    /* Encode data that exceeds initial capacity */
    for (int i = 0; i < 100; i++) {
        ASSERT_OK(tlv_encode_uint32(enc, TAG_COUNT, (uint32_t)i));
    }
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    printf("  Encoded 100 fields, total size: %zu bytes\n", len);
    
    /* Decode and verify */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    for (int i = 0; i < 100; i++) {
        ASSERT_OK(tlv_decode_next(dec, &field));
        uint32_t value = tlv_field_as_uint32(&field);
        ASSERT_EQ(value, (uint32_t)i);
    }
    
    printf("  All 100 fields verified\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * FIELD SEARCH TEST
 * ========================================================================= */

void test_find_field() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    
    /* Encode multiple fields */
    ASSERT_OK(tlv_encode_uint32(enc, 0x0001, 10));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0002, 20));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0003, 30));
    ASSERT_OK(tlv_encode_string(enc, 0x0010, "target"));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0004, 40));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    /* Search for specific field */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    ASSERT_OK(tlv_find_field(dec, 0x0010, &field));
    ASSERT_EQ(field.tag, 0x0010);
    
    const char* str = tlv_field_get_string(&field);
    ASSERT_TRUE(strcmp(str, "target") == 0);
    
    printf("  Found target field: \"%s\"\n", str);
    
    /* Search for non-existent field */
    tlv_decoder_reset(dec);
    distric_err_t err = tlv_find_field(dec, 0x9999, &field);
    ASSERT_TRUE(err == DISTRIC_ERR_NOT_FOUND);
    
    printf("  Non-existent field correctly not found\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * FORWARD COMPATIBILITY TEST (unknown field skipping)
 * ========================================================================= */

void test_forward_compatibility() {
    TEST_START();
    
    /* Simulate old encoder creating message with known fields */
    tlv_encoder_t* enc = tlv_encoder_create(256);
    ASSERT_OK(tlv_encode_uint32(enc, TAG_TERM, 1));
    ASSERT_OK(tlv_encode_string(enc, TAG_NODE_ID, "node-1"));
    
    /* Simulate new encoder adding unknown field (type 0xFF) */
    ASSERT_OK(tlv_encode_raw(enc, (tlv_type_t)0xFF, 0x9999, 
                            (const uint8_t*)"future_data", 11));
    
    ASSERT_OK(tlv_encode_uint32(enc, TAG_COUNT, 100));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    /* Old decoder should skip unknown field */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    /* Read field 1 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_EQ(field.tag, TAG_TERM);
    
    /* Read field 2 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_EQ(field.tag, TAG_NODE_ID);
    
    /* Read field 3 (unknown type, but decoder doesn't care) */
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_EQ(field.tag, 0x9999);
    ASSERT_EQ(field.type, 0xFF);
    
    /* Read field 4 */
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_EQ(field.tag, TAG_COUNT);
    ASSERT_EQ(tlv_field_as_uint32(&field), 100);
    
    printf("  Old decoder successfully skipped unknown field\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * VALIDATION TEST
 * ========================================================================= */

void test_buffer_validation() {
    TEST_START();
    
    /* Valid buffer */
    tlv_encoder_t* enc = tlv_encoder_create(256);
    ASSERT_OK(tlv_encode_uint32(enc, TAG_TERM, 42));
    ASSERT_OK(tlv_encode_string(enc, TAG_NODE_ID, "test"));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    ASSERT_TRUE(tlv_validate_buffer(buffer, len));
    printf("  Valid buffer accepted\n");
    
    /* Corrupt buffer (truncate) */
    ASSERT_TRUE(!tlv_validate_buffer(buffer, len - 5));
    printf("  Truncated buffer rejected\n");
    
    /* Corrupt buffer (wrong length in header) */
    uint8_t corrupt[256];
    memcpy(corrupt, buffer, len);
    
    /* Modify length field in first TLV header (offset 3-6) */
    *(uint32_t*)(corrupt + 3) = htonl(9999);
    
    ASSERT_TRUE(!tlv_validate_buffer(corrupt, len));
    printf("  Corrupted length field detected\n");
    
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * ZERO-COPY VERIFICATION
 * ========================================================================= */

void test_zero_copy_decoding() {
    TEST_START();
    
    const char* test_string = "This is a test string for zero-copy verification";
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    ASSERT_OK(tlv_encode_string(enc, TAG_MESSAGE, test_string));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    /* Decode */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    const char* decoded = tlv_field_get_string(&field);
    
    /* Verify pointer is within original buffer (zero-copy) */
    ASSERT_TRUE(decoded >= (const char*)buffer);
    ASSERT_TRUE(decoded < (const char*)(buffer + len));
    
    printf("  Zero-copy verified: decoded pointer is within buffer\n");
    printf("  Buffer range: %p - %p\n", (void*)buffer, (void*)(buffer + len));
    printf("  Decoded ptr:  %p\n", (void*)decoded);
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * EDGE CASES
 * ========================================================================= */

void test_empty_string() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(64);
    ASSERT_OK(tlv_encode_string(enc, TAG_MESSAGE, ""));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    const char* str = tlv_field_get_string(&field);
    ASSERT_TRUE(str != NULL);
    ASSERT_TRUE(strlen(str) == 0);
    
    printf("  Empty string handled correctly\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_zero_length_bytes() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(64);
    ASSERT_OK(tlv_encode_bytes(enc, TAG_DATA, NULL, 0));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    size_t data_len;
    const uint8_t* data = tlv_field_get_bytes(&field, &data_len);
    ASSERT_TRUE(data_len == 0);
    
    printf("  Zero-length byte array handled correctly\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_encoder_reset() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    
    /* Encode first message */
    ASSERT_OK(tlv_encode_uint32(enc, TAG_TERM, 1));
    size_t len1 = tlv_encoder_size(enc);
    
    /* Reset */
    tlv_encoder_reset(enc);
    ASSERT_EQ(tlv_encoder_size(enc), 0);
    
    /* Encode second message */
    ASSERT_OK(tlv_encode_uint32(enc, TAG_TERM, 2));
    size_t len2 = tlv_encoder_size(enc);
    
    ASSERT_EQ(len1, len2);  /* Same size */
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    /* Verify second message */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_EQ(tlv_field_as_uint32(&field), 2);
    
    printf("  Encoder reset works correctly\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_decoder_reset() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    ASSERT_OK(tlv_encode_uint32(enc, 0x0001, 10));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0002, 20));
    ASSERT_OK(tlv_encode_uint32(enc, 0x0003, 30));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    /* Read all fields */
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    /* Should be at EOF */
    ASSERT_TRUE(!tlv_decoder_has_more(dec));
    
    /* Reset and read again */
    tlv_decoder_reset(dec);
    ASSERT_TRUE(tlv_decoder_has_more(dec));
    
    ASSERT_OK(tlv_decode_next(dec, &field));
    ASSERT_EQ(field.tag, 0x0001);
    
    printf("  Decoder reset works correctly\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

void test_type_mismatch_detection() {
    TEST_START();
    
    tlv_encoder_t* enc = tlv_encoder_create(64);
    ASSERT_OK(tlv_encode_string(enc, TAG_MESSAGE, "not a number"));
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    ASSERT_OK(tlv_decode_next(dec, &field));
    
    /* Try to extract as uint32 (should fail) */
    uint32_t value;
    distric_err_t err = tlv_field_get_uint32(&field, &value);
    ASSERT_TRUE(err == DISTRIC_ERR_TYPE_MISMATCH);
    
    printf("  Type mismatch correctly detected\n");
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * PERFORMANCE TEST
 * ========================================================================= */

void test_performance() {
    TEST_START();
    
    const int COUNT = 10000;
    
    tlv_encoder_t* enc = tlv_encoder_create(1024 * 64);
    
    /* Encode 10,000 fields */
    for (int i = 0; i < COUNT; i++) {
        tlv_encode_uint32(enc, TAG_COUNT, (uint32_t)i);
    }
    
    size_t len;
    uint8_t* buffer = tlv_encoder_finalize(enc, &len);
    
    printf("  Encoded %d fields in %zu bytes\n", COUNT, len);
    printf("  Average bytes per field: %.2f\n", (double)len / COUNT);
    
    /* Decode 10,000 fields */
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    tlv_field_t field;
    
    int decoded_count = 0;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        decoded_count++;
    }
    
    ASSERT_EQ(decoded_count, COUNT);
    printf("  Decoded %d fields successfully\n", decoded_count);
    
    tlv_decoder_free(dec);
    tlv_encoder_free(enc);
    
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Protocol - TLV Encoder/Decoder Tests ===\n");
    
    /* Basic tests */
    test_encode_decode_uint32();
    test_encode_decode_string();
    test_encode_decode_bytes();
    test_encode_all_numeric_types();
    
    /* Multiple fields */
    test_multiple_fields();
    
    /* Buffer management */
    test_buffer_growth();
    
    /* Field search */
    test_find_field();
    
    /* Compatibility */
    test_forward_compatibility();
    
    /* Validation */
    test_buffer_validation();
    
    /* Zero-copy */
    test_zero_copy_decoding();
    
    /* Edge cases */
    test_empty_string();
    test_zero_length_bytes();
    test_encoder_reset();
    test_decoder_reset();
    test_type_mismatch_detection();
    
    /* Performance */
    test_performance();
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\n✓ All TLV tests passed!\n");
        printf("✓ Session 2.2 COMPLETE - Ready for Session 2.3 (Message Definitions)\n");
        printf("\nKey Features Verified:\n");
        printf("  - Encode/decode round-trip for all types\n");
        printf("  - Dynamic buffer growth\n");
        printf("  - Zero-copy decoding\n");
        printf("  - Forward compatibility (unknown field skipping)\n");
        printf("  - Buffer validation\n");
        printf("  - Type safety\n");
        printf("  - Performance: ~11 bytes per uint32 field (header + value)\n");
    }
    
    return tests_failed > 0 ? 1 : 0;
}