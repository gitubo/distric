/**
 * @file gossip_internal.h
 * @brief Internal structures and functions for gossip protocol
 * 
 * This header is NOT part of the public API.
 * Contains internal state structures, membership management, and protocol internals.
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#ifndef DISTRIC_GOSSIP_INTERNAL_H
#define DISTRIC_GOSSIP_INTERNAL_H

#include "distric_gossip.h"
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/**
 * @brief Internal membership entry with metadata
 */
typedef struct {
    gossip_node_info_t info;       /**< Public node information */
    uint64_t suspect_time_ms;      /**< When node was marked suspected */
    uint32_t transmit_count;       /**< Gossip transmissions remaining */
    bool local_node;               /**< Is this the local node? */
} membership_entry_t;

/**
 * @brief Main gossip state structure
 */
struct gossip_state_s {
    /* Configuration */
    gossip_config_t config;
    
    /* Network */
    int udp_socket;                    /**< UDP socket for gossip */
    
    /* Membership */
    membership_entry_t* membership;    /**< Dynamic array of nodes */
    size_t membership_count;           /**< Current node count */
    size_t membership_capacity;        /**< Allocated capacity */
    pthread_mutex_t membership_lock;   /**< Protects membership array */
    
    /* Self state */
    uint64_t self_incarnation;         /**< Our incarnation number */
    uint64_t self_load;                /**< Our current load */
    
    /* Protocol state */
    atomic_uint_fast32_t sequence_counter; /**< Message sequence numbers */
    atomic_bool running;               /**< Is protocol running? */
    pthread_t protocol_thread;         /**< Main protocol thread */
    pthread_t receiver_thread;         /**< Message receiver thread */
    
    /* Callbacks */
    gossip_node_joined_fn on_joined;
    void* on_joined_data;
    gossip_node_suspected_fn on_suspected;
    void* on_suspected_data;
    gossip_node_failed_fn on_failed;
    void* on_failed_data;
    gossip_node_recovered_fn on_recovered;
    void* on_recovered_data;
    
    /* Metrics (optional) */
    void* ping_sent_metric;
    void* ping_recv_metric;
    void* suspicions_metric;
    void* failures_metric;
};

/* ============================================================================
 * MEMBERSHIP MANAGEMENT FUNCTIONS
 * ========================================================================= */

/**
 * @brief Find a membership entry by node ID
 * 
 * @param state Gossip state
 * @param node_id Node ID to find
 * @return Pointer to entry or NULL if not found
 * 
 * Note: Caller must hold membership_lock
 */
membership_entry_t* membership_find(gossip_state_t* state, const char* node_id);

/**
 * @brief Add a new node to membership
 * 
 * @param state Gossip state
 * @param info Node information
 * @return Pointer to new entry or NULL on error
 * 
 * Note: Caller must hold membership_lock
 */
membership_entry_t* membership_add(gossip_state_t* state, const gossip_node_info_t* info);

/**
 * @brief Remove a node from membership
 * 
 * @param state Gossip state
 * @param node_id Node ID to remove
 * 
 * Note: Caller must hold membership_lock
 */
void membership_remove(gossip_state_t* state, const char* node_id);

/**
 * @brief Update an existing membership entry
 * 
 * @param state Gossip state
 * @param info Updated node information
 * 
 * Note: Caller must hold membership_lock
 */
void membership_update(gossip_state_t* state, const gossip_node_info_t* info);

/* ============================================================================
 * STATE TRANSITION FUNCTIONS
 * ========================================================================= */

/**
 * @brief Mark a node as alive
 * 
 * @param state Gossip state
 * @param node_id Node ID
 */
void gossip_mark_node_alive(gossip_state_t* state, const char* node_id);

/**
 * @brief Mark a node as suspected
 * 
 * @param state Gossip state
 * @param node_id Node ID
 */
void gossip_mark_node_suspected(gossip_state_t* state, const char* node_id);

/**
 * @brief Mark a node as failed
 * 
 * @param state Gossip state
 * @param node_id Node ID
 */
void gossip_mark_node_failed(gossip_state_t* state, const char* node_id);

/**
 * @brief Handle a node's graceful departure
 * 
 * @param state Gossip state
 * @param node_id Node ID
 */
void gossip_handle_node_leave(gossip_state_t* state, const char* node_id);

/* ============================================================================
 * PROTOCOL INTERNALS
 * ========================================================================= */

/**
 * @brief Check for suspicion timeouts and transition to failed
 * 
 * @param state Gossip state
 */
void gossip_check_suspicion_timeouts(gossip_state_t* state);

/**
 * @brief Get updates that need to be broadcast
 * 
 * @param state Gossip state
 * @param updates_out Array of node updates (caller must free)
 * @param count_out Number of updates
 */
void gossip_get_updates_to_broadcast(
    gossip_state_t* state,
    gossip_node_info_t** updates_out,
    uint32_t* count_out
);

/**
 * @brief Update node state from received message
 * 
 * @param state Gossip state
 * @param msg Received message
 * @param src_addr Source address string
 * @param src_port Source port
 */
void gossip_update_node_from_message(
    gossip_state_t* state,
    const struct gossip_message_s* msg,
    const char* src_addr,
    uint16_t src_port
);

/* ============================================================================
 * PROTOCOL THREADS
 * ========================================================================= */

/**
 * @brief Main protocol thread (periodic probing)
 * 
 * @param arg Gossip state
 * @return NULL
 */
void* gossip_protocol_thread(void* arg);

/**
 * @brief Receiver thread (handles incoming messages)
 * 
 * @param arg Gossip state
 * @return NULL
 */
void* gossip_receiver_thread(void* arg);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_GOSSIP_INTERNAL_H */