/*
 * fuzz_auth.c — libFuzzer harness for HMAC authentication paths
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -O1 -I../include \
 *         fuzz_auth.c ../src/auth.c ../src/error_info.c -o fuzz_auth
 */

#include "distric_protocol/auth.h"
#include "distric_protocol/binary.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Fixed key for reproducibility */
static const uint8_t FUZZ_KEY[32] = {
    0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE,
    0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,
    0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10,
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < MESSAGE_HEADER_SIZE) return 0;

    distric_auth_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.key, FUZZ_KEY, sizeof(FUZZ_KEY));
    ctx.key_len = sizeof(FUZZ_KEY);
    ctx.mac_fn  = NULL;   /* use built-in HMAC-SHA256 */

    const uint8_t *header_buf  = data;
    const uint8_t *payload     = data + MESSAGE_HEADER_SIZE;
    size_t         payload_len = size - MESSAGE_HEADER_SIZE;

    uint8_t mac[DISTRIC_AUTH_MAC_SIZE];
    uint8_t expected[DISTRIC_AUTH_MAC_SIZE];

    /* Compute must never crash regardless of input */
    distric_auth_compute(&ctx, header_buf, payload, payload_len, mac);

    /* Verify with random expected (mostly should fail; must not crash) */
    memcpy(expected, data, sizeof(expected) < size ? sizeof(expected) : size);
    distric_auth_verify(&ctx, header_buf, payload, payload_len, expected);

    return 0;
}