/**
 * @file gossip_conflict_resolution.c
 * @brief Enhanced Conflict Resolution for Section 4.2
 * 
 * Implements explicit state priority ordering and enhanced refutation
 * mechanisms as recommended in the alignment report.
 * 
 * @version 1.1
 * @date 2026-02-12
 */

#include "distric_gossip.h"
#include "distric_gossip/gossip_internal.h"
#include <string.h>

/* ============================================================================
 * STATE PRIORITY ORDERING
 * ========================================================================= */

/**
 * @brief Get priority value for a node status
 * 
 * Higher values indicate higher priority in conflict resolution.
 * Priority order: ALIVE > SUSPECTED > FAILED > LEFT
 * 
 * @param status Node status
 * @return Priority value (3=highest, 0=lowest, -1=invalid)
 */
static int get_state_priority(gossip_node_status_t status) {
    switch (status) {
        case GOSSIP_NODE_ALIVE:     return 3;
        case GOSSIP_NODE_SUSPECTED: return 2;
        case GOSSIP_NODE_FAILED:    return 1;
        case GOSSIP_NODE_LEFT:      return 0;
        default:                    return -1;
    }
}

/**
 * @brief Compare two node status updates for conflict resolution
 * 
 * Returns:
 *  > 0 if update1 should win
 *  < 0 if update2 should win
 *  = 0 if they are equivalent
 * 
 * Resolution rules:
 * 1. Higher incarnation always wins
 * 2. If incarnation equal, higher state priority wins (ALIVE > SUSPECTED > FAILED)
 * 
 * @param status1 First status
 * @param incarnation1 First incarnation
 * @param status2 Second status
 * @param incarnation2 Second incarnation
 * @return Comparison result
 */
int gossip_compare_updates(
    gossip_node_status_t status1,
    uint64_t incarnation1,
    gossip_node_status_t status2,
    uint64_t incarnation2
) {
    /* Rule 1: Higher incarnation wins */
    if (incarnation1 > incarnation2) {
        return 1;
    }
    if (incarnation1 < incarnation2) {
        return -1;
    }
    
    /* Rule 2: If incarnation equal, higher state priority wins */
    int priority1 = get_state_priority(status1);
    int priority2 = get_state_priority(status2);
    
    return priority1 - priority2;
}

/**
 * @brief Determine if an incoming update should replace current state
 * 
 * @param current_status Current node status
 * @param current_incarnation Current incarnation
 * @param update_status Incoming update status
 * @param update_incarnation Incoming update incarnation
 * @return true if update should be applied, false otherwise
 */
bool gossip_should_apply_update(
    gossip_node_status_t current_status,
    uint64_t current_incarnation,
    gossip_node_status_t update_status,
    uint64_t update_incarnation
) {
    return gossip_compare_updates(
        update_status, update_incarnation,
        current_status, current_incarnation
    ) > 0;
}

/* ============================================================================
 * ENHANCED REFUTATION WITH BROADCAST
 * ========================================================================= */

/**
 * @brief Broadcast refutation immediately to all known nodes
 * 
 * This is more aggressive than piggybacking and ensures faster
 * propagation of refutation. Use when immediate correction is critical.
 * 
 * @param state Gossip state
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t gossip_broadcast_refutation(gossip_state_t* state) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Increment incarnation */
    state->self_incarnation++;
    
    /* Update self in membership */
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* self = membership_find(state, state->config.node_id);
    if (!self) {
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    self->info.incarnation = state->self_incarnation;
    self->info.status = GOSSIP_NODE_ALIVE;
    self->transmit_count = state->config.max_transmissions;
    
    /* Prepare refutation message */
    gossip_message_t refutation_msg;
    memset(&refutation_msg, 0, sizeof(refutation_msg));
    
    refutation_msg.type = GOSSIP_MSG_MEMBERSHIP;
    strncpy(refutation_msg.sender_id, state->config.node_id, 
            sizeof(refutation_msg.sender_id) - 1);
    refutation_msg.incarnation = state->self_incarnation;
    refutation_msg.sequence = atomic_fetch_add(&state->sequence_counter, 1);
    
    /* Include self update */
    refutation_msg.update_count = 1;
    refutation_msg.updates = malloc(sizeof(gossip_node_info_t));
    if (!refutation_msg.updates) {
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    memcpy(&refutation_msg.updates[0], &self->info, sizeof(gossip_node_info_t));
    
    /* Get list of all nodes to broadcast to */
    size_t target_count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (!state->membership[i].local_node && 
            state->membership[i].info.status != GOSSIP_NODE_FAILED &&
            state->membership[i].info.status != GOSSIP_NODE_LEFT) {
            target_count++;
        }
    }
    
    if (target_count == 0) {
        free(refutation_msg.updates);
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_OK;  /* No one to broadcast to */
    }
    
    /* Allocate target list */
    gossip_node_info_t* targets = malloc(target_count * sizeof(gossip_node_info_t));
    if (!targets) {
        free(refutation_msg.updates);
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Copy targets */
    size_t target_idx = 0;
    for (size_t i = 0; i < state->membership_count && target_idx < target_count; i++) {
        if (!state->membership[i].local_node && 
            state->membership[i].info.status != GOSSIP_NODE_FAILED &&
            state->membership[i].info.status != GOSSIP_NODE_LEFT) {
            memcpy(&targets[target_idx++], &state->membership[i].info, 
                   sizeof(gossip_node_info_t));
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
    
    /* Serialize message once */
    uint8_t buffer[8192];
    size_t bytes_written = 0;
    int serialize_result = gossip_message_serialize(&refutation_msg, buffer, 
                                                    sizeof(buffer), &bytes_written);
    
    free(refutation_msg.updates);
    
    if (serialize_result != 0) {
        free(targets);
        return DISTRIC_ERR_SERIALIZATION;
    }
    
    /* Broadcast to all targets */
    size_t sent_count = 0;
    size_t failed_count = 0;
    
    for (size_t i = 0; i < target_count; i++) {
        struct sockaddr_in dest_addr;
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(targets[i].port);
        
        if (inet_pton(AF_INET, targets[i].address, &dest_addr.sin_addr) != 1) {
            failed_count++;
            continue;
        }
        
        ssize_t sent = sendto(state->udp_socket, buffer, bytes_written, 0,
                             (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        
        if (sent > 0) {
            sent_count++;
        } else {
            failed_count++;
        }
    }
    
    free(targets);
    
    /* Log broadcast result */
    if (state->config.logger) {
        logger_t* logger = (logger_t*)state->config.logger;
        char sent_str[32], failed_str[32];
        snprintf(sent_str, sizeof(sent_str), "%zu", sent_count);
        snprintf(failed_str, sizeof(failed_str), "%zu", failed_count);
        
        LOG_INFO(logger, "gossip", "Broadcast refutation",
                "incarnation", NULL,
                "sent", sent_str,
                "failed", failed_str,
                NULL);
    }
    
    return sent_count > 0 ? DISTRIC_OK : DISTRIC_ERR_IO;
}

/**
 * @brief Get statistics about conflict resolution events
 * 
 * @param state Gossip state
 * @param refutations_out Number of refutations performed
 * @param conflicts_resolved_out Number of conflicts resolved
 * @return DISTRIC_OK on success
 */
distric_err_t gossip_get_conflict_stats(
    gossip_state_t* state,
    uint64_t* refutations_out,
    uint64_t* conflicts_resolved_out
) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* These would be tracked in state if we add counters */
    /* For now, return placeholder values */
    if (refutations_out) {
        *refutations_out = 0;  /* TODO: Track in state */
    }
    if (conflicts_resolved_out) {
        *conflicts_resolved_out = 0;  /* TODO: Track in state */
    }
    
    return DISTRIC_OK;
}

/* ============================================================================
 * CONFLICT RESOLUTION IN UPDATE HANDLING
 * ========================================================================= */

/**
 * @brief Apply conflict resolution rules when processing membership update
 * 
 * This function should be called by gossip_handle_membership_update()
 * to determine if an incoming update should be applied.
 * 
 * @param state Gossip state
 * @param node_id Node being updated
 * @param update_status New status
 * @param update_incarnation New incarnation
 * @return true if update should be applied, false if rejected
 */
bool gossip_resolve_conflict(
    gossip_state_t* state,
    const char* node_id,
    gossip_node_status_t update_status,
    uint64_t update_incarnation
) {
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    if (!entry) {
        /* New node - always accept */
        pthread_mutex_unlock(&state->membership_lock);
        return true;
    }
    
    /* Check if this is our local node */
    if (entry->local_node) {
        /* Check if we're being suspected/failed */
        if (update_status == GOSSIP_NODE_SUSPECTED || 
            update_status == GOSSIP_NODE_FAILED) {
            
            /* Auto-refute if incoming incarnation >= ours */
            if (update_incarnation >= entry->info.incarnation) {
                /* Will be refuted by gossip_mark_node_suspected */
                pthread_mutex_unlock(&state->membership_lock);
                return false;  /* Reject the update, we'll refute */
            }
        }
    }
    
    /* Apply standard conflict resolution rules */
    bool should_apply = gossip_should_apply_update(
        entry->info.status,
        entry->info.incarnation,
        update_status,
        update_incarnation
    );
    
    pthread_mutex_unlock(&state->membership_lock);
    
    return should_apply;
}