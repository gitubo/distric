/**
 * @file binary.c
 * @brief Binary protocol header — serialization, validation, CRC32
 *
 * Changes from baseline:
 *  - validate_message_header: version check is now major-byte only (item 7).
 *    Minor-version differences are tolerated; only major mismatches reject.
 *  - validate_message_header: payload_len bound now uses PROTOCOL_MAX_PAYLOAD_SIZE
 *    (configurable, default 64 MiB) rather than a hard-coded 10 MiB constant.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_protocol/binary.h"
#include "distric_protocol/crc32.h"

#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <arpa/inet.h>

/* ============================================================================
 * PORTABLE ntohll / htonll
 * ========================================================================= */

#ifndef ntohll
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline uint64_t ntohll(uint64_t v) {
    return (((uint64_t)ntohl((uint32_t)(v & 0xFFFFFFFFu))) << 32)
         |  ((uint64_t)ntohl((uint32_t)(v >> 32)));
}
static inline uint64_t htonll(uint64_t v) { return ntohll(v); }
#else
static inline uint64_t ntohll(uint64_t v) { return v; }
static inline uint64_t htonll(uint64_t v) { return v; }
#endif
#endif

/* ============================================================================
 * INTERNAL: TIMESTAMP + MESSAGE-ID
 * ========================================================================= */

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static atomic_uint_fast64_t message_id_counter = ATOMIC_VAR_INIT(0);

static uint64_t generate_message_id(void) {
    uint64_t ts      = get_timestamp_us() / 1000000ULL; /* seconds */
    uint64_t counter = atomic_fetch_add_explicit(&message_id_counter, 1,
                                                  memory_order_relaxed);
    return (ts << 32) | (counter & 0xFFFFFFFFu);
}

/* ============================================================================
 * HEADER INITIALIZATION
 * ========================================================================= */

distric_err_t message_header_init(
    message_header_t* header,
    message_type_t    msg_type,
    uint32_t          payload_len
) {
    if (!header) return DISTRIC_ERR_INVALID_ARG;

    memset(header, 0, sizeof(*header));
    header->magic       = PROTOCOL_MAGIC;
    header->version     = PROTOCOL_VERSION;
    header->msg_type    = (uint16_t)msg_type;
    header->flags       = MSG_FLAG_NONE;
    header->reserved    = 0;
    header->payload_len = payload_len;
    header->message_id  = generate_message_id();
    header->timestamp_us= (uint32_t)(get_timestamp_us() & 0xFFFFFFFFu);
    header->crc32       = 0; /* computed separately */

    return DISTRIC_OK;
}

/* ============================================================================
 * SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t*                buf
) {
    if (!header || !buf) return DISTRIC_ERR_INVALID_ARG;

    uint32_t* b32 = (uint32_t*)buf;
    uint16_t* b16 = (uint16_t*)buf;
    uint64_t* b64 = (uint64_t*)buf;

    b32[0] = htonl(header->magic);
    b16[2] = htons(header->version);
    b16[3] = htons(header->msg_type);
    b16[4] = htons(header->flags);
    b16[5] = htons(header->reserved);
    b32[3] = htonl(header->payload_len);
    b64[2] = htonll(header->message_id);
    b32[6] = htonl(header->timestamp_us);
    b32[7] = htonl(header->crc32);

    return DISTRIC_OK;
}

/* ============================================================================
 * DESERIALIZATION
 * ========================================================================= */

distric_err_t deserialize_header(
    const uint8_t*   buf,
    message_header_t* header
) {
    if (!buf || !header) return DISTRIC_ERR_INVALID_ARG;

    const uint32_t* b32 = (const uint32_t*)buf;
    const uint16_t* b16 = (const uint16_t*)buf;
    const uint64_t* b64 = (const uint64_t*)buf;

    header->magic        = ntohl(b32[0]);
    header->version      = ntohs(b16[2]);
    header->msg_type     = ntohs(b16[3]);
    header->flags        = ntohs(b16[4]);
    header->reserved     = ntohs(b16[5]);
    header->payload_len  = ntohl(b32[3]);
    header->message_id   = ntohll(b64[2]);
    header->timestamp_us = ntohl(b32[6]);
    header->crc32        = ntohl(b32[7]);

    return DISTRIC_OK;
}

/* ============================================================================
 * VALIDATION (item 3 + item 7)
 * ========================================================================= */

bool validate_message_header(const message_header_t* header) {
    if (!header) return false;

    /* Magic */
    if (header->magic != PROTOCOL_MAGIC) return false;

    /* --- Version negotiation (item 7) ---
     * The version field encodes major in the high byte, minor in the low byte.
     *   version = (major << 8) | minor
     * A major-version mismatch (different high byte) is a hard rejection.
     * A minor-version difference is acceptable — the receiver degrades
     * gracefully by ignoring unknown optional fields.
     */
    uint8_t our_major  = (uint8_t)(PROTOCOL_VERSION >> 8);
    uint8_t peer_major = (uint8_t)(header->version   >> 8);
    if (peer_major != our_major) return false;

    /* Message type must be in a defined subsystem range */
    if (header->msg_type == 0 || header->msg_type > 0x4FFFu) return false;

    /* --- Payload size limit (item 3) ---
     * Use PROTOCOL_MAX_PAYLOAD_SIZE, which is compile-time overridable.
     * This prevents trivially large allocations before application-level
     * checks (e.g., RPC_MAX_MESSAGE_SIZE) have a chance to fire.
     */
    if (header->payload_len > PROTOCOL_MAX_PAYLOAD_SIZE) return false;

    /* Reserved must be zero */
    if (header->reserved != 0) return false;

    return true;
}

/* ============================================================================
 * MESSAGE TYPE TO STRING
 * ========================================================================= */

const char* message_type_to_string(message_type_t t) {
    switch (t) {
        case MSG_RAFT_REQUEST_VOTE:              return "RAFT_REQUEST_VOTE";
        case MSG_RAFT_REQUEST_VOTE_RESPONSE:     return "RAFT_REQUEST_VOTE_RESPONSE";
        case MSG_RAFT_APPEND_ENTRIES:            return "RAFT_APPEND_ENTRIES";
        case MSG_RAFT_APPEND_ENTRIES_RESPONSE:   return "RAFT_APPEND_ENTRIES_RESPONSE";
        case MSG_RAFT_INSTALL_SNAPSHOT:          return "RAFT_INSTALL_SNAPSHOT";
        case MSG_RAFT_INSTALL_SNAPSHOT_RESPONSE: return "RAFT_INSTALL_SNAPSHOT_RESPONSE";
        case MSG_GOSSIP_PING:                    return "GOSSIP_PING";
        case MSG_GOSSIP_ACK:                     return "GOSSIP_ACK";
        case MSG_GOSSIP_INDIRECT_PING:           return "GOSSIP_INDIRECT_PING";
        case MSG_GOSSIP_MEMBERSHIP_UPDATE:       return "GOSSIP_MEMBERSHIP_UPDATE";
        case MSG_GOSSIP_SUSPECT:                 return "GOSSIP_SUSPECT";
        case MSG_GOSSIP_ALIVE:                   return "GOSSIP_ALIVE";
        case MSG_TASK_ASSIGNMENT:                return "TASK_ASSIGNMENT";
        case MSG_TASK_RESULT:                    return "TASK_RESULT";
        case MSG_TASK_STATUS:                    return "TASK_STATUS";
        case MSG_TASK_CANCEL:                    return "TASK_CANCEL";
        case MSG_TASK_HEARTBEAT:                 return "TASK_HEARTBEAT";
        case MSG_CLIENT_SUBMIT:                  return "CLIENT_SUBMIT";
        case MSG_CLIENT_RESPONSE:                return "CLIENT_RESPONSE";
        case MSG_CLIENT_QUERY:                   return "CLIENT_QUERY";
        case MSG_CLIENT_ERROR:                   return "CLIENT_ERROR";
        default:                                 return "UNKNOWN";
    }
}

/* ============================================================================
 * CRC32
 * ========================================================================= */

distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t*    payload,
    size_t            payload_len
) {
    if (!header) return DISTRIC_ERR_INVALID_ARG;
    if (payload_len > 0 && !payload) return DISTRIC_ERR_INVALID_ARG;

    /* Zero CRC field before computation */
    header->crc32 = 0;

    uint8_t hbuf[MESSAGE_HEADER_SIZE];
    distric_err_t err = serialize_header(header, hbuf);
    if (err != DISTRIC_OK) return err;

    uint32_t crc = 0xFFFFFFFFu;
    crc = compute_crc32_incremental(hbuf, MESSAGE_HEADER_SIZE, crc);
    if (payload && payload_len > 0)
        crc = compute_crc32_incremental(payload, payload_len, crc);
    header->crc32 = ~crc;

    return DISTRIC_OK;
}

bool verify_message_crc32(
    const message_header_t* header,
    const uint8_t*          payload,
    size_t                  payload_len
) {
    if (!header) return false;

    message_header_t tmp = *header;
    uint32_t orig_crc    = tmp.crc32;

    if (compute_header_crc32(&tmp, payload, payload_len) != DISTRIC_OK)
        return false;

    return tmp.crc32 == orig_crc;
}