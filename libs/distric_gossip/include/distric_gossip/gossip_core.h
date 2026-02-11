/**
 * @file gossip_internal.h
 * @brief Internal structures and functions for gossip protocol
 * 
 * This header is NOT part of the public API.
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#ifndef GOSSIP_INTERNAL_H
#define GOSSIP_INTERNAL_H

#include "distric_gossip.h"
#include <pthread.h>
#include <stdatomic.h>

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
 * INTERNAL FUNCTION DECLARATIONS
 * ========================================================================= */

/* Membership management */
membership_entry_t* membership_find(gossip_state_t* state, const char* node_id);
membership_entry_t* membership_add(gossip_state_t* state, const gossip_node_info_t* info);
void membership_remove(gossip_state_t* state, const char* node_id);
void membership_update(gossip_state_t* state, const gossip_node_info_t* info);

/* State transitions */
void gossip_mark_node_alive(gossip_state_t* state, const char* node_id);
void gossip_mark_node_suspected(gossip_state_t* state, const char* node_id);
void gossip_mark_node_failed(gossip_state_t* state, const char* node_id);
void gossip_handle_node_leave(gossip_state_t* state, const char* node_id);

/* Suspicion timeout checking */
void gossip_check_suspicion_timeouts(gossip_state_t* state);

/* Update dissemination */
void gossip_get_updates_to_broadcast(
    gossip_state_t* state,
    gossip_node_info_t** updates_out,
    uint32_t* count_out
);

void gossip_update_node_from_message(
    gossip_state_t* state,
    const struct gossip_message_s* msg,
    const char* src_addr,
    uint16_t src_port
);

/* Forward declaration for message type */
struct gossip_message_s;

/* Protocol threads */
void* gossip_protocol_thread(void* arg);
void* gossip_receiver_thread(void* arg);

#endif /* GOSSIP_INTERNAL_H */