#ifndef DISTRIC_PROTOCOL_CAPABILITY_H
#define DISTRIC_PROTOCOL_CAPABILITY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_obs.h>   /* distric_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Capability bitmap flags ---- */
#define CAP_COMPRESSION   (1u << 0)   /* payload compression supported        */
#define CAP_AEAD          (1u << 1)   /* AEAD encryption supported            */
#define CAP_STREAMING     (1u << 2)   /* chunked / streaming messages         */
#define CAP_INDEXED_TLV   (1u << 3)   /* indexed TLV decoder available        */
#define CAP_HMAC_AUTH     (1u << 4)   /* HMAC-SHA256 authentication           */

/* ---- Capability message ---- */
typedef struct {
    uint8_t  major_version;       /* PROTOCOL_VERSION >> 8                    */
    uint8_t  minor_version;       /* PROTOCOL_VERSION & 0xFF                  */
    uint32_t capabilities;        /* CAP_* bitmap of features this node has   */
    char     node_id[64];         /* optional node identifier (may be empty)  */
} capability_msg_t;

/* ---- Wire field tags (internal) ---- */
#define CAP_TAG_MAJOR     0x0101u
#define CAP_TAG_MINOR     0x0102u
#define CAP_TAG_CAPS      0x0103u
#define CAP_TAG_NODE_ID   0x0104u

/*
 * Serialise a capability_msg_t into a heap-allocated TLV buffer.
 * Caller must free(*buf_out).
 */
distric_err_t capability_serialize(
    const capability_msg_t *cap,
    uint8_t               **buf_out,
    size_t                 *len_out);

/*
 * Deserialise a TLV payload into a capability_msg_t.
 */
distric_err_t capability_deserialize(
    const uint8_t  *buf,
    size_t          len,
    capability_msg_t *cap_out);

/*
 * Negotiate capabilities between this node and a peer.
 *
 * Rules:
 *   - Major versions MUST match; returns DISTRIC_ERR_INVALID_FORMAT otherwise.
 *   - negotiated_caps = local->capabilities & peer->capabilities (intersection).
 *
 * negotiated_caps_out receives the resulting bitmap; both nodes should use it.
 */
distric_err_t capability_negotiate(
    const capability_msg_t *local,
    const capability_msg_t *peer,
    uint32_t               *negotiated_caps_out);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_CAPABILITY_H */