#ifndef DISTRIC_PROTOCOL_AUTH_H
#define DISTRIC_PROTOCOL_AUTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_obs.h>   /* distric_err_t */

#ifdef __cplusplus
extern "C" {
#endif

#define DISTRIC_AUTH_MAC_SIZE  32u   /* HMAC-SHA256 output length */
#define DISTRIC_AUTH_KEY_MAX   64u   /* max key length in bytes   */

/*
 * Pluggable MAC function.
 * Signature matches HMAC-SHA256 so callers can substitute e.g. OpenSSL.
 * Must be thread-safe (stateless or internally synchronised).
 */
typedef void (*distric_mac_fn)(
    const uint8_t *key,  size_t key_len,
    const uint8_t *data, size_t data_len,
    uint8_t        out[DISTRIC_AUTH_MAC_SIZE]);

/*
 * Authentication context.
 * Zero-initialise → authentication disabled (ctx.key_len == 0 ⇒ no-op).
 */
typedef struct {
    uint8_t        key[DISTRIC_AUTH_KEY_MAX];
    size_t         key_len;
    distric_mac_fn mac_fn;    /* NULL → use built-in HMAC-SHA256 */
} distric_auth_ctx_t;

/*
 * Compute a 32-byte MAC over the serialised header (32 bytes) concatenated
 * with the message payload.  Writes result to mac_out.
 *
 * If ctx->key_len == 0 the function is a no-op and returns DISTRIC_OK.
 */
distric_err_t distric_auth_compute(
    const distric_auth_ctx_t *ctx,
    const uint8_t            *header_buf,   /* serialised header, 32 bytes */
    const uint8_t            *payload,
    size_t                    payload_len,
    uint8_t                   mac_out[DISTRIC_AUTH_MAC_SIZE]);

/*
 * Constant-time MAC verification.
 * Returns DISTRIC_OK on match, DISTRIC_ERR_INVALID_FORMAT on mismatch.
 * If ctx->key_len == 0 always returns DISTRIC_OK (authentication bypassed).
 */
distric_err_t distric_auth_verify(
    const distric_auth_ctx_t *ctx,
    const uint8_t            *header_buf,
    const uint8_t            *payload,
    size_t                    payload_len,
    const uint8_t             expected_mac[DISTRIC_AUTH_MAC_SIZE]);

/*
 * Built-in HMAC-SHA256 (pure C11, no external dependency).
 * Can be assigned to distric_auth_ctx_t.mac_fn.
 */
void distric_hmac_sha256(
    const uint8_t *key,  size_t key_len,
    const uint8_t *data, size_t data_len,
    uint8_t        out[DISTRIC_AUTH_MAC_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_AUTH_H */