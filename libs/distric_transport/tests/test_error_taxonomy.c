/**
 * @file test_error_taxonomy.c
 * @brief Unit tests — transport error classification and conversion.
 *
 * Covers:
 *  1. transport_classify_errno() maps known errno values correctly.
 *  2. transport_err_str() never returns NULL.
 *  3. transport_err_to_distric() produces valid distric_err_t values.
 *  4. Unknown errno maps to TRANSPORT_INTERNAL.
 *  5. EAGAIN/EWOULDBLOCK map to TRANSPORT_WOULD_BLOCK.
 *  6. ECONNRESET/EPIPE map to TRANSPORT_RESET.
 *  7. ETIMEDOUT maps to TRANSPORT_TIMEOUT.
 *  8. ENOMEM maps to TRANSPORT_RESOURCE.
 */

#include <distric_transport.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b) do {                                              \
    if ((a) != (b)) {                                                     \
        fprintf(stderr, "FAIL %s:%d: " #a " = %d, expected %d\n",        \
                __FILE__, __LINE__, (int)(a), (int)(b));                  \
        tests_failed++;                                                    \
        return;                                                            \
    }                                                                     \
} while(0)

#define ASSERT_NOT_NULL(p) do {                                           \
    if (!(p)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: " #p " is NULL\n",                  \
                __FILE__, __LINE__);                                       \
        tests_failed++;                                                    \
        return;                                                            \
    }                                                                     \
} while(0)

#define TEST_START() printf("[TEST] %s\n", __func__)
#define TEST_PASS()  do { printf("[PASS] %s\n", __func__); tests_passed++; } while(0)

/* ============================================================================
 * TEST CASES
 * ========================================================================= */

void test_classify_would_block(void) {
    TEST_START();
    ASSERT_EQ(transport_classify_errno(EAGAIN),       TRANSPORT_WOULD_BLOCK);
    ASSERT_EQ(transport_classify_errno(EWOULDBLOCK),  TRANSPORT_WOULD_BLOCK);
    TEST_PASS();
}

void test_classify_reset(void) {
    TEST_START();
    ASSERT_EQ(transport_classify_errno(ECONNRESET),   TRANSPORT_RESET);
    ASSERT_EQ(transport_classify_errno(EPIPE),        TRANSPORT_RESET);
    ASSERT_EQ(transport_classify_errno(ECONNABORTED), TRANSPORT_RESET);
    TEST_PASS();
}

void test_classify_timeout(void) {
    TEST_START();
    ASSERT_EQ(transport_classify_errno(ETIMEDOUT),    TRANSPORT_TIMEOUT);
    TEST_PASS();
}

void test_classify_address(void) {
    TEST_START();
    ASSERT_EQ(transport_classify_errno(ECONNREFUSED), TRANSPORT_ADDRESS);
    ASSERT_EQ(transport_classify_errno(EHOSTUNREACH), TRANSPORT_ADDRESS);
    ASSERT_EQ(transport_classify_errno(ENETUNREACH),  TRANSPORT_ADDRESS);
    ASSERT_EQ(transport_classify_errno(EADDRINUSE),   TRANSPORT_ADDRESS);
    TEST_PASS();
}

void test_classify_resource(void) {
    TEST_START();
    ASSERT_EQ(transport_classify_errno(ENOMEM),       TRANSPORT_RESOURCE);
    ASSERT_EQ(transport_classify_errno(EMFILE),       TRANSPORT_RESOURCE);
    ASSERT_EQ(transport_classify_errno(ENFILE),       TRANSPORT_RESOURCE);
    ASSERT_EQ(transport_classify_errno(ENOBUFS),      TRANSPORT_RESOURCE);
    TEST_PASS();
}

void test_classify_unknown_is_internal(void) {
    TEST_START();
    /* EIO should not match any specific category above */
    ASSERT_EQ(transport_classify_errno(EIO),          TRANSPORT_INTERNAL);
    /* Very large value: also internal */
    ASSERT_EQ(transport_classify_errno(9999),         TRANSPORT_INTERNAL);
    TEST_PASS();
}

void test_err_str_never_null(void) {
    TEST_START();
    for (int i = 0; i <= TRANSPORT_INTERNAL; i++) {
        const char* s = transport_err_str((transport_err_t)i);
        ASSERT_NOT_NULL(s);
        ASSERT_NOT_NULL(s[0] != '\0' ? s : NULL);  /* Non-empty */
    }
    /* Out-of-range: still not NULL */
    ASSERT_NOT_NULL(transport_err_str((transport_err_t)999));
    TEST_PASS();
}

void test_to_distric_valid(void) {
    TEST_START();
    /* TRANSPORT_OK → DISTRIC_OK */
    ASSERT_EQ(transport_err_to_distric(TRANSPORT_OK),           DISTRIC_OK);
    /* TRANSPORT_WOULD_BLOCK → DISTRIC_OK (non-fatal, not an error) */
    ASSERT_EQ(transport_err_to_distric(TRANSPORT_WOULD_BLOCK),  DISTRIC_OK);
    /* TRANSPORT_BACKPRESSURE → DISTRIC_ERR_BACKPRESSURE */
    ASSERT_EQ(transport_err_to_distric(TRANSPORT_BACKPRESSURE), DISTRIC_ERR_BACKPRESSURE);
    /* TRANSPORT_PEER_CLOSED → DISTRIC_ERR_EOF */
    ASSERT_EQ(transport_err_to_distric(TRANSPORT_PEER_CLOSED),  DISTRIC_ERR_EOF);
    /* TRANSPORT_TIMEOUT → DISTRIC_ERR_TIMEOUT */
    ASSERT_EQ(transport_err_to_distric(TRANSPORT_TIMEOUT),      DISTRIC_ERR_TIMEOUT);
    TEST_PASS();
}

void test_rate_limited_code(void) {
    TEST_START();
    /* TRANSPORT_RATE_LIMITED must map to a non-OK distric code */
    distric_err_t de = transport_err_to_distric(TRANSPORT_RATE_LIMITED);
    ASSERT_EQ(de != DISTRIC_OK ? 1 : 0, 1);
    printf("    TRANSPORT_RATE_LIMITED → distric_err_t %d\n", de);
    TEST_PASS();
}

/* ============================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    printf("=== DistriC Transport — Error Taxonomy Tests ===\n\n");

    test_classify_would_block();
    test_classify_reset();
    test_classify_timeout();
    test_classify_address();
    test_classify_resource();
    test_classify_unknown_is_internal();
    test_err_str_never_null();
    test_to_distric_valid();
    test_rate_limited_code();

    printf("\n=== Results: Passed=%d  Failed=%d ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}