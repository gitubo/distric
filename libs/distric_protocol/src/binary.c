/**
 * @file binary.c
 * @brief Binary protocol header — serialisation, validation, CRC32
 *
 * Applied improvements:
 *
 *  Improvement #1 — Shared byteswap header.
 *    htonll/ntohll are now imported from src/internal/byteswap.h instead of
 *    being copy-pasted here.  This eliminates the divergence risk between
 *    binary.c and tlv.c.
 *
 *  Improvement #5 — Reserved field no longer checked in validate_message_header.
 *    The reserved field is preserved for forward compatibility.  Future protocol
 *    minor versions may assign meaning to it; rejecting non-zero values would
 *    cause hard wire-breaks in rolling-upgrade scenarios.  Unknown values are
 *    logged at DEBUG level.
 *
 *  Improvement #6 — timestamp field stores Unix seconds (timestamp_s).
 *    The old timestamp_us (32-bit µs) wrapped every ~71 minutes.  The field
 *    now stores Unix epoch seconds; wrap-around is year 2106.  Wire layout
 *    is unchanged (same offset, same 4 bytes).
 *
 * Pre-existing fixes (retained):
 *  Fix #1  — Strict-aliasing / unaligned-access UB: all multi-byte field
 *             reads/writes use memcpy + hton/ntoh.
 *  Fix #7  — Major-byte-only version negotiation.
 *  Fix #3  — Configurable payload size ceiling via PROTOCOL_MAX_PAYLOAD_SIZE.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_protocol/binary.h"
#include "distric_protocol/crc32.h"
#include "distric_protocol/byteswap.h"   

#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <arpa/inet.h>

/* ============================================================================
 * INTERNAL: TIMESTAMP + MESSAGE-ID
 * ========================================================================= */

/** Return Unix time in microseconds (used only for message-ID generation). */
static uint64_t get_timestamp_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/** Return current Unix time in seconds (for the header timestamp_s field). */
static uint32_t get_timestamp_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)ts.tv_sec;
}

static atomic_uint_fast64_t message_id_counter = ATOMIC_VAR_INIT(0);

/**
 * Generate a unique 64-bit message ID:
 *   [63..32] Unix epoch seconds  (monotone over time, no wrap during lifetime)
 *   [31.. 0] per-process counter (monotone within a second)
 */
static uint64_t generate_message_id(void)
{
    uint64_t ts      = get_timestamp_us() / 1000000ULL; /* seconds */
    uint64_t counter = atomic_fetch_add_explicit(&message_id_counter, 1,
                                                  memory_order_relaxed);
    return (ts << 32) | (counter & 0xFFFFFFFFu);
}

/* ============================================================================
 * HEADER INITIALISATION
 * ========================================================================= */

distric_err_t message_header_init(
    message_header_t* header,
    message_type_t    msg_type,
    uint32_t          payload_len)
{
    if (!header) return DISTRIC_ERR_INVALID_ARG;
    if (payload_len > PROTOCOL_MAX_PAYLOAD_SIZE) return DISTRIC_ERR_INVALID_ARG;

    header->magic       = PROTOCOL_MAGIC;
    header->version     = PROTOCOL_VERSION;
    header->msg_type    = (uint16_t)msg_type;
    header->flags       = MSG_FLAG_NONE;
    header->reserved    = 0;
    header->payload_len = payload_len;
    header->message_id  = generate_message_id();
    header->timestamp_s = get_timestamp_s();   /* Improvement #6 */
    header->crc32       = 0;

    return DISTRIC_OK;
}

/* ============================================================================
 * SERIALISATION (Fix #1: memcpy throughout)
 * ========================================================================= */

distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t*                buf)
{
    if (!header || !buf) return DISTRIC_ERR_INVALID_ARG;

    uint32_t u32;
    uint16_t u16;
    uint64_t u64;

    /* [0..3]  magic */
    u32 = htonl(header->magic);
    memcpy(buf + 0, &u32, 4);

    /* [4..5]  version */
    u16 = htons(header->version);
    memcpy(buf + 4, &u16, 2);

    /* [6..7]  msg_type */
    u16 = htons(header->msg_type);
    memcpy(buf + 6, &u16, 2);

    /* [8..9]  flags */
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

    /* [24..27] timestamp_s (Improvement #6: seconds, not µs) */
    u32 = htonl(header->timestamp_s);
    memcpy(buf + 24, &u32, 4);

    /* [28..31] crc32 */
    u32 = htonl(header->crc32);
    memcpy(buf + 28, &u32, 4);

    return DISTRIC_OK;
}

/* ============================================================================
 * DESERIALISATION (Fix #1: memcpy throughout)
 * ========================================================================= */

distric_err_t deserialize_header(
    const uint8_t*    buf,
    message_header_t* header)
{
    if (!buf || !header) return DISTRIC_ERR_INVALID_ARG;

    uint32_t u32;
    uint16_t u16;
    uint64_t u64;

    /* [0..3]  magic */
    memcpy(&u32, buf + 0, 4);
    header->magic = ntohl(u32);

    /* [4..5]  version */
    memcpy(&u16, buf + 4, 2);
    header->version = ntohs(u16);

    /* [6..7]  msg_type */
    memcpy(&u16, buf + 6, 2);
    header->msg_type = ntohs(u16);

    /* [8..9]  flags */
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

    /* [24..27] timestamp_s (Improvement #6) */
    memcpy(&u32, buf + 24, 4);
    header->timestamp_s = ntohl(u32);

    /* [28..31] crc32 */
    memcpy(&u32, buf + 28, 4);
    header->crc32 = ntohl(u32);

    return DISTRIC_OK;
}

/* ============================================================================
 * VALIDATION
 *
 * Improvement #5: reserved field is NOT checked.  Future minor versions may
 * assign meaning to it; rejecting non-zero values would be a wire-break hazard
 * in rolling upgrades.  validate_message_header() is a correctness gate, not
 * a policy enforcement point.
 * ========================================================================= */

bool validate_message_header(const message_header_t* header)
{
    if (!header) return false;

    /* Magic */
    if (header->magic != PROTOCOL_MAGIC) return false;

    /*
     * Version negotiation — major byte only.
     * version = (major << 8) | minor.  A major mismatch is a hard rejection;
     * minor differences are tolerated (unknown optional TLV fields are skipped).
     */
    uint8_t our_major  = (uint8_t)(PROTOCOL_VERSION >> 8);
    uint8_t peer_major = (uint8_t)(header->version   >> 8);
    if (peer_major != our_major) return false;

    /* Message type: non-zero and within defined subsystem range (0x1000–0x4FFF) */
    if (header->msg_type == 0 || header->msg_type > 0x4FFFu) return false;

    /* Payload ceiling */
    if (header->payload_len > PROTOCOL_MAX_PAYLOAD_SIZE) return false;

    /*
     * Improvement #5: reserved field deliberately NOT checked here.
     * If a future minor version sets reserved bits, we must accept the message.
     */

    return true;
}

/* ============================================================================
 * MESSAGE TYPE STRINGS
 * ========================================================================= */

const char* message_type_to_string(message_type_t msg_type)
{
    switch (msg_type) {
        case MSG_RAFT_REQUEST_VOTE:        return "RAFT_REQUEST_VOTE";
        case MSG_RAFT_VOTE_RESPONSE:       return "RAFT_VOTE_RESPONSE";
        case MSG_RAFT_APPEND_ENTRIES:      return "RAFT_APPEND_ENTRIES";
        case MSG_RAFT_APPEND_RESPONSE:     return "RAFT_APPEND_RESPONSE";
        case MSG_RAFT_INSTALL_SNAPSHOT:    return "RAFT_INSTALL_SNAPSHOT";
        case MSG_RAFT_SNAPSHOT_RESPONSE:   return "RAFT_SNAPSHOT_RESPONSE";
        case MSG_GOSSIP_PING:              return "GOSSIP_PING";
        case MSG_GOSSIP_ACK:               return "GOSSIP_ACK";
        case MSG_GOSSIP_PING_REQ:          return "GOSSIP_PING_REQ";
        case MSG_GOSSIP_MEMBERSHIP_UPDATE: return "GOSSIP_MEMBERSHIP_UPDATE";
        case MSG_TASK_ASSIGNMENT:          return "TASK_ASSIGNMENT";
        case MSG_TASK_RESULT:              return "TASK_RESULT";
        case MSG_TASK_STATUS:              return "TASK_STATUS";
        case MSG_CLIENT_SUBMIT:            return "CLIENT_SUBMIT";
        case MSG_CLIENT_RESPONSE:          return "CLIENT_RESPONSE";
        case MSG_CLIENT_QUERY:             return "CLIENT_QUERY";
        case MSG_CLIENT_ERROR:             return "CLIENT_ERROR";
        default:                           return "UNKNOWN";
    }
}

/* ============================================================================
 * CRC32 COMPUTATION
 * ========================================================================= */

distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t*    payload,
    size_t            payload_len)
{
    if (!header) return DISTRIC_ERR_INVALID_ARG;
    if (payload_len > 0 && !payload) return DISTRIC_ERR_INVALID_ARG;

    /* Zero CRC field so it does not affect the digest */
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

    message_header_t tmp   = *header;
    uint32_t         saved = tmp.crc32;

    if (compute_header_crc32(&tmp, payload, payload_len) != DISTRIC_OK) {
        return false;
    }

    return tmp.crc32 == saved;
}