/**
 * @file messages.h
 * @brief SWIM protocol message types and structures
 * 
 * This header is NOT part of the public API.
 * Contains message type definitions for the SWIM protocol.
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#ifndef DISTRIC_GOSSIP_MESSAGES_H
#define DISTRIC_GOSSIP_MESSAGES_H

#include "distric_gossip.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MESSAGE TYPES
 * ========================================================================= */

/**
 * @brief SWIM protocol message types
 */
typedef enum {
    GOSSIP_MSG_PING = 1,       /**< Direct health check probe */
    GOSSIP_MSG_ACK,            /**< Acknowledgment response */
    GOSSIP_MSG_PING_REQ,       /**< Indirect ping request */
    GOSSIP_MSG_MEMBERSHIP,     /**< Piggybacked membership updates */
    GOSSIP_MSG_LEAVE           /**< Graceful departure notification */
} gossip_msg_type_t;

/**
 * @brief SWIM protocol message structure
 * 
 * All SWIM messages contain:
 * - Message type
 * - Sender identification
 * - Sequence number (for request/response matching)
 * - Optional piggybacked membership updates
 */
typedef struct gossip_message_s {
    gossip_msg_type_t type;        /**< Message type */
    char sender_id[64];            /**< Sender's node ID */
    uint64_t incarnation;          /**< Sender's incarnation number */
    uint32_t sequence;             /**< Sequence number for matching */
    
    /* Piggybacked membership updates */
    gossip_node_info_t* updates;   /**< Array of membership updates */
    uint32_t update_count;         /**< Number of updates */
} gossip_message_t;

/* ============================================================================
 * MESSAGE SERIALIZATION
 * ========================================================================= */

/**
 * @brief Serialize a message to buffer
 * 
 * @param msg Message to serialize
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @param bytes_written Number of bytes written
 * @return 0 on success, -1 on error
 */
int gossip_message_serialize(
    const gossip_message_t* msg,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* bytes_written
);

/**
 * @brief Deserialize a message from buffer
 * 
 * @param buffer Input buffer
 * @param buffer_size Size of input buffer
 * @param msg Output message (caller must free msg->updates)
 * @return 0 on success, -1 on error
 */
int gossip_message_deserialize(
    const uint8_t* buffer,
    size_t buffer_size,
    gossip_message_t* msg
);

/**
 * @brief Free resources allocated in a message
 * 
 * @param msg Message to clean up
 */
void gossip_message_free(gossip_message_t* msg);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_GOSSIP_MESSAGES_H */