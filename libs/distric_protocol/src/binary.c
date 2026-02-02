/**
 * @file binary.c
 * @brief Binary Protocol Header Implementation
 * 
 * Provides serialization, deserialization, and validation for the
 * fixed 32-byte message header with network byte order conversion.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/binary.h"
#include "distric_protocol/crc32.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <arpa/inet.h>  /* htonl, htons, ntohl, ntohs */

/* ============================================================================
 * BYTE ORDER CONVERSION (for 64-bit values)
 * ========================================================================= */

/** Convert 64-bit host to network byte order */
static inline uint64_t htonll(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32) | 
           htonl((uint32_t)(value >> 32));
#else
    return value;
#endif
}

/** Convert 64-bit network to host byte order */
static inline uint64_t ntohll(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)ntohl((uint32_t)(value & 0xFFFFFFFF)) << 32) | 
           ntohl((uint32_t)(value >> 32));
#else
    return value;
#endif
}

/* ============================================================================
 * TIMESTAMP HELPERS
 * ========================================================================= */

/**
 * @brief Get current timestamp in microseconds
 */
static uint64_t get_timestamp_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ============================================================================
 * MESSAGE ID GENERATION
 * ========================================================================= */

/** Atomic counter for message ID generation */
static _Atomic uint64_t message_id_counter = 0;

/**
 * @brief Generate unique message ID
 * 
 * Combines timestamp (upper 32 bits) with atomic counter (lower 32 bits)
 * to ensure uniqueness even across restarts.
 */
static uint64_t generate_message_id(void) {
    uint64_t timestamp = get_timestamp_us() / 1000000ULL;  /* Convert to seconds for ID */
    uint64_t counter = atomic_fetch_add(&message_id_counter, 1);
    
    /* Combine: [timestamp:32][counter:32] */
    return (timestamp << 32) | (counter & 0xFFFFFFFF);
}

/* ============================================================================
 * HEADER INITIALIZATION
 * ========================================================================= */

distric_err_t message_header_init(
    message_header_t* header,
    message_type_t msg_type,
    uint32_t payload_len
) {
    if (!header) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    memset(header, 0, sizeof(message_header_t));
    
    header->magic = PROTOCOL_MAGIC;
    header->version = PROTOCOL_VERSION;
    header->msg_type = msg_type;
    header->flags = MSG_FLAG_NONE;
    header->reserved = 0;
    header->payload_len = payload_len;
    header->message_id = generate_message_id();
    
    /* Store lower 32 bits of microsecond timestamp */
    uint64_t now_us = get_timestamp_us();
    header->timestamp_us = (uint32_t)(now_us & 0xFFFFFFFF);
    
    header->crc32 = 0;  /* Computed separately */
    
    return DISTRIC_OK;
}

/* ============================================================================
 * SERIALIZATION / DESERIALIZATION
 * ========================================================================= */

distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t* buffer
) {
    if (!header || !buffer) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Convert to network byte order and write to buffer */
    uint32_t* buf32 = (uint32_t*)buffer;
    uint16_t* buf16 = (uint16_t*)buffer;
    uint64_t* buf64 = (uint64_t*)buffer;
    
    /* Offset 0: magic (4 bytes) */
    buf32[0] = htonl(header->magic);
    
    /* Offset 4: version (2 bytes) */
    buf16[2] = htons(header->version);
    
    /* Offset 6: msg_type (2 bytes) */
    buf16[3] = htons(header->msg_type);
    
    /* Offset 8: flags (2 bytes) */
    buf16[4] = htons(header->flags);
    
    /* Offset 10: reserved (2 bytes) */
    buf16[5] = htons(header->reserved);
    
    /* Offset 12: payload_len (4 bytes) */
    buf32[3] = htonl(header->payload_len);
    
    /* Offset 16: message_id (8 bytes) */
    buf64[2] = htonll(header->message_id);
    
    /* Offset 24: timestamp_us (4 bytes) */
    buf32[6] = htonl(header->timestamp_us);
    
    /* Offset 28: crc32 (4 bytes) */
    buf32[7] = htonl(header->crc32);
    
    return DISTRIC_OK;
}

distric_err_t deserialize_header(
    const uint8_t* buffer,
    message_header_t* header
) {
    if (!buffer || !header) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Convert from network byte order */
    const uint32_t* buf32 = (const uint32_t*)buffer;
    const uint16_t* buf16 = (const uint16_t*)buffer;
    const uint64_t* buf64 = (const uint64_t*)buffer;
    
    header->magic = ntohl(buf32[0]);
    header->version = ntohs(buf16[2]);
    header->msg_type = ntohs(buf16[3]);
    header->flags = ntohs(buf16[4]);
    header->reserved = ntohs(buf16[5]);
    header->payload_len = ntohl(buf32[3]);
    header->message_id = ntohll(buf64[2]);
    header->timestamp_us = ntohl(buf32[6]);
    header->crc32 = ntohl(buf32[7]);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

bool validate_message_header(const message_header_t* header) {
    if (!header) {
        return false;
    }
    
    /* Check magic number */
    if (header->magic != PROTOCOL_MAGIC) {
        return false;
    }
    
    /* Check version (currently only support 0x0001) */
    if (header->version != PROTOCOL_VERSION) {
        return false;
    }
    
    /* Check message type is in valid range */
    if (header->msg_type == 0 || header->msg_type > 0x4FFF) {
        return false;
    }
    
    /* Check payload length is reasonable (max 10MB) */
    if (header->payload_len > 10 * 1024 * 1024) {
        return false;
    }
    
    /* Reserved field must be zero */
    if (header->reserved != 0) {
        return false;
    }
    
    return true;
}

/* ============================================================================
 * CRC32 COMPUTATION
 * ========================================================================= */

distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t* payload,
    size_t payload_len
) {
    if (!header) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Validate payload_len matches header */
    if (payload_len != header->payload_len) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Zero out CRC field before computation */
    uint32_t saved_crc = header->crc32;
    header->crc32 = 0;
    
    /* Serialize header to buffer */
    uint8_t header_buf[MESSAGE_HEADER_SIZE];
    distric_err_t err = serialize_header(header, header_buf);
    if (err != DISTRIC_OK) {
        header->crc32 = saved_crc;
        return err;
    }
    
    /* Compute CRC over header */
    uint32_t crc = 0xFFFFFFFF;
    crc = compute_crc32_incremental(header_buf, MESSAGE_HEADER_SIZE, crc);
    
    /* Compute CRC over payload if present */
    if (payload && payload_len > 0) {
        crc = compute_crc32_incremental(payload, payload_len, crc);
    }
    
    /* Final inversion */
    crc = ~crc;
    
    /* Store CRC in header */
    header->crc32 = crc;
    
    return DISTRIC_OK;
}

bool verify_message_crc32(
    const message_header_t* header,
    const uint8_t* payload,
    size_t payload_len
) {
    if (!header) {
        return false;
    }
    
    /* Make a copy to compute CRC */
    message_header_t temp = *header;
    uint32_t original_crc = temp.crc32;
    
    /* Recompute CRC */
    distric_err_t err = compute_header_crc32(&temp, payload, payload_len);
    if (err != DISTRIC_OK) {
        return false;
    }
    
    /* Compare */
    return temp.crc32 == original_crc;
}

/* ============================================================================
 * MESSAGE TYPE TO STRING
 * ========================================================================= */

const char* message_type_to_string(message_type_t msg_type) {
    switch (msg_type) {
        /* Raft messages */
        case MSG_RAFT_REQUEST_VOTE:
            return "RAFT_REQUEST_VOTE";
        case MSG_RAFT_REQUEST_VOTE_RESPONSE:
            return "RAFT_REQUEST_VOTE_RESPONSE";
        case MSG_RAFT_APPEND_ENTRIES:
            return "RAFT_APPEND_ENTRIES";
        case MSG_RAFT_APPEND_ENTRIES_RESPONSE:
            return "RAFT_APPEND_ENTRIES_RESPONSE";
        case MSG_RAFT_INSTALL_SNAPSHOT:
            return "RAFT_INSTALL_SNAPSHOT";
        case MSG_RAFT_INSTALL_SNAPSHOT_RESPONSE:
            return "RAFT_INSTALL_SNAPSHOT_RESPONSE";
        
        /* Gossip messages */
        case MSG_GOSSIP_PING:
            return "GOSSIP_PING";
        case MSG_GOSSIP_ACK:
            return "GOSSIP_ACK";
        case MSG_GOSSIP_INDIRECT_PING:
            return "GOSSIP_INDIRECT_PING";
        case MSG_GOSSIP_MEMBERSHIP_UPDATE:
            return "GOSSIP_MEMBERSHIP_UPDATE";
        case MSG_GOSSIP_SUSPECT:
            return "GOSSIP_SUSPECT";
        case MSG_GOSSIP_ALIVE:
            return "GOSSIP_ALIVE";
        
        /* Task messages */
        case MSG_TASK_ASSIGNMENT:
            return "TASK_ASSIGNMENT";
        case MSG_TASK_RESULT:
            return "TASK_RESULT";
        case MSG_TASK_STATUS:
            return "TASK_STATUS";
        case MSG_TASK_CANCEL:
            return "TASK_CANCEL";
        case MSG_TASK_HEARTBEAT:
            return "TASK_HEARTBEAT";
        
        /* Client messages */
        case MSG_CLIENT_SUBMIT:
            return "CLIENT_SUBMIT";
        case MSG_CLIENT_RESPONSE:
            return "CLIENT_RESPONSE";
        case MSG_CLIENT_QUERY:
            return "CLIENT_QUERY";
        case MSG_CLIENT_ERROR:
            return "CLIENT_ERROR";
        
        default:
            return "UNKNOWN";
    }
}