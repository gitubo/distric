/**
 * @file binary.c
 * @brief Binary protocol header — serialization, validation, CRC32
 *
 * Fix #1 — Strict-aliasing / unaligned-access UB:
 *   The original code cast uint8_t* wire buffers directly to uint16_t*,
 *   uint32_t*, and uint64_t* for reading and writing multi-byte fields.
 *   This violates C99 strict-aliasing rules and causes undefined behaviour on
 *   any architecture that enforces alignment (ARM, RISC-V, SPARC, MIPS).
 *   Even on x86 the compiler may legally miscompile such code at -O2.
 *
 *   All multi-byte reads/writes now use memcpy() with local integer variables
 *   and htonX/ntohX byte-swap helpers.  GCC/Clang optimise this to a single
 *   load/store instruction on alignment-permissive targets, so there is zero
 *   runtime overhead.
 *
 * Fix #7 (existing) — Major-byte-only version negotiation:
 *   Minor-version differences are tolerated.
 *
 * Fix #3 (existing) — Configurable payload size ceiling via
 *   PROTOCOL_MAX_PAYLOAD_SIZE.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_protocol/binary.h"
#include "distric_protocol/crc32.h"

#include <string.h>   /* memcpy */
#include <stdatomic.h>
#include <time.h>
#include <arpa/inet.h>

/* ============================================================================
 * PORTABLE 64-BIT BYTE-SWAP
 * ========================================================================= */

#ifndef ntohll
# if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline uint64_t ntohll(uint64_t v)
{
    return (((uint64_t)ntohl((uint32_t)(v & 0xFFFFFFFFu))) << 32)
         | ((uint64_t)ntohl((uint32_t)(v >> 32)));
}
static inline uint64_t htonll(uint64_t v) { return ntohll(v); }
# else
static inline uint64_t ntohll(uint64_t v) { return v; }
static inline uint64_t htonll(uint64_t v) { return v; }
# endif
#endif

/* ============================================================================
 * INTERNAL: TIMESTAMP + MESSAGE-ID
 * ========================================================================= */

static uint64_t get_timestamp_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static atomic_uint_fast64_t message_id_counter = ATOMIC_VAR_INIT(0);

static uint64_t generate_message_id(void)
{
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
    uint32_t          payload_len)
{
    if (!header) return DISTRIC_ERR_INVALID_ARG;

    memset(header, 0, sizeof(*header));
    header->magic        = PROTOCOL_MAGIC;
    header->version      = PROTOCOL_VERSION;
    header->msg_type     = (uint16_t)msg_type;
    header->flags        = MSG_FLAG_NONE;
    header->reserved     = 0;
    header->payload_len  = payload_len;
    header->message_id   = generate_message_id();
    header->timestamp_us = (uint32_t)(get_timestamp_us() & 0xFFFFFFFFu);
    header->crc32        = 0; /* computed separately */

    return DISTRIC_OK;
}

/* ============================================================================
 * SERIALISATION — Fix #1
 *
 * Wire layout (32 bytes, big-endian):
 *   [0..3]   magic        uint32
 *   [4..5]   version      uint16
 *   [6..7]   msg_type     uint16
 *   [8..9]   flags        uint16
 *   [10..11] reserved     uint16
 *   [12..15] payload_len  uint32
 *   [16..23] message_id   uint64
 *   [24..27] timestamp_us uint32
 *   [28..31] crc32        uint32
 * ========================================================================= */

distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t*                buf)
{
    if (!header || !buf) return DISTRIC_ERR_INVALID_ARG;

    uint32_t u32;
    uint16_t u16;
    uint64_t u64;

    /* [0..3] magic */
    u32 = htonl(header->magic);
    memcpy(buf + 0, &u32, 4);

    /* [4..5] version */
    u16 = htons(header->version);
    memcpy(buf + 4, &u16, 2);

    /* [6..7] msg_type */
    u16 = htons(header->msg_type);
    memcpy(buf + 6, &u16, 2);

    /* [8..9] flags */
    u16 = htons(header->flags);
    memcpy(buf + 8, &u16, 2);

    /* [10..11] reserved */
    u16 = htons(header->reserved);
    memcpy(buf + 10, &u16, 2);

    /* [12..15] payload_len */
    u32 = htonl(header->payload_len);
    memcpy(buf + 12, &u32, 4);

    /* [16..23] message_id */
    u64 = htonll(header->message_id);
    memcpy(buf + 16, &u64, 8);

    /* [24..27] timestamp_us */
    u32 = htonl(header->timestamp_us);
    memcpy(buf + 24, &u32, 4);

    /* [28..31] crc32 */
    u32 = htonl(header->crc32);
    memcpy(buf + 28, &u32, 4);

    return DISTRIC_OK;
}

/* ============================================================================
 * DESERIALISATION — Fix #1
 * ========================================================================= */

distric_err_t deserialize_header(
    const uint8_t*    buf,
    message_header_t* header)
{
    if (!buf || !header) return DISTRIC_ERR_INVALID_ARG;

    uint32_t u32;
    uint16_t u16;
    uint64_t u64;

    /* [0..3] magic */
    memcpy(&u32, buf + 0, 4);
    header->magic = ntohl(u32);

    /* [4..5] version */
    memcpy(&u16, buf + 4, 2);
    header->version = ntohs(u16);

    /* [6..7] msg_type */
    memcpy(&u16, buf + 6, 2);
    header->msg_type = ntohs(u16);

    /* [8..9] flags */
    memcpy(&u16, buf + 8, 2);
    header->flags = ntohs(u16);

    /* [10..11] reserved */
    memcpy(&u16, buf + 10, 2);
    header->reserved = ntohs(u16);

    /* [12..15] payload_len */
    memcpy(&u32, buf + 12, 4);
    header->payload_len = ntohl(u32);

    /* [16..23] message_id */
    memcpy(&u64, buf + 16, 8);
    header->message_id = ntohll(u64);

    /* [24..27] timestamp_us */
    memcpy(&u32, buf + 24, 4);
    header->timestamp_us = ntohl(u32);

    /* [28..31] crc32 */
    memcpy(&u32, buf + 28, 4);
    header->crc32 = ntohl(u32);

    return DISTRIC_OK;
}

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

bool validate_message_header(const message_header_t* header)
{
    if (!header) return false;

    /* Magic */
    if (header->magic != PROTOCOL_MAGIC) return false;

    /*
     * Version negotiation — major-byte-only check (item 7).
     * version = (major << 8) | minor
     * A major mismatch (different high byte) is a hard rejection.
     * A minor-version difference is tolerated: unknown optional fields
     * are skipped by the TLV layer.
     */
    uint8_t our_major  = (uint8_t)(PROTOCOL_VERSION >> 8);
    uint8_t peer_major = (uint8_t)(header->version   >> 8);
    if (peer_major != our_major) return false;

    /* Message type must be non-zero and within defined subsystem range */
    if (header->msg_type == 0 || header->msg_type > 0x4FFFu) return false;

    /*
     * Payload size ceiling (item 3).
     * PROTOCOL_MAX_PAYLOAD_SIZE is compile-time overridable; default 64 MiB.
     */
    if (header->payload_len > PROTOCOL_MAX_PAYLOAD_SIZE) return false;

    /* Reserved must be zero (forward-compat: unknown flags cause rejection) */
    if (header->reserved != 0) return false;

    return true;
}

/* ============================================================================
 * MESSAGE TYPE → STRING
 * ========================================================================= */

const char* message_type_to_string(message_type_t t)
{
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
 * CRC32 OVER HEADER + PAYLOAD
 * ========================================================================= */

distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t*    payload,
    size_t            payload_len)
{
    if (!header) return DISTRIC_ERR_INVALID_ARG;
    if (payload_len > 0 && !payload) return DISTRIC_ERR_INVALID_ARG;

    /* Zero CRC field before computation so it doesn't affect the digest */
    header->crc32 = 0;

    uint8_t hbuf[MESSAGE_HEADER_SIZE];
    distric_err_t err = serialize_header(header, hbuf);
    if (err != DISTRIC_OK) return err;

    uint32_t crc = 0xFFFFFFFFu;
    crc = compute_crc32_incremental(hbuf, MESSAGE_HEADER_SIZE, crc);
    if (payload && payload_len > 0) {
        crc = compute_crc32_incremental(payload, payload_len, crc);
    }
    header->crc32 = ~crc;

    return DISTRIC_OK;
}

bool verify_message_crc32(
    const message_header_t* header,
    const uint8_t*          payload,
    size_t                  payload_len)
{
    if (!header) return false;

    message_header_t tmp   = *header;      /* shallow copy */
    uint32_t         saved = tmp.crc32;

    if (compute_header_crc32(&tmp, payload, payload_len) != DISTRIC_OK) {
        return false;
    }

    return tmp.crc32 == saved;
}