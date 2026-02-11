/**
 * @file gossip_membership.c
 * @brief Membership list management and state transitions
 * 
 * Manages the cluster membership list including:
 * - Adding/removing nodes
 * - State transitions (ALIVE -> SUSPECTED -> FAILED)
 * - Conflict resolution via incarnation numbers
 * - Update dissemination
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#include "distric_gossip/gossip_core.h"
#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ============================================================================
 * HELPER FUNCTIONS
 * ========================================================================= */

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* ============================================================================
 * MEMBERSHIP ARRAY MANAGEMENT
 * ========================================================================= */

/**
 * @brief Find a node in the membership list
 * @note Caller must hold membership_lock
 */
membership_entry_t* membership_find(gossip_state_t* state, const char* node_id) {
    for (size_t i = 0; i < state->membership_count; i++) {
        if (strcmp(state->membership[i].info.node_id, node_id) == 0) {
            return &state->membership[i];
        }
    }
    return NULL;
}

/**
 * @brief Add a new node to the membership list
 * @note Caller must hold membership_lock
 */
membership_entry_t* membership_add(gossip_state_t* state, const gossip_node_info_t* info) {
    /* Check if we need to grow the array */
    if (state->membership_count >= state->membership_capacity) {
        size_t new_capacity = state->membership_capacity == 0 ? 16 : state->membership_capacity * 2;
        membership_entry_t* new_array = (membership_entry_t*)realloc(
            state->membership,
            new_capacity * sizeof(membership_entry_t)
        );
        
        if (!new_array) {
            return NULL;
        }
        
        state->membership = new_array;
        state->membership_capacity = new_capacity;
    }
    
    /* Add new entry */
    membership_entry_t* entry = &state->membership[state->membership_count];
    memset(entry, 0, sizeof(membership_entry_t));
    memcpy(&entry->info, info, sizeof(gossip_node_info_t));
    entry->info.last_seen_ms = get_time_ms();
    entry->transmit_count = state->config.max_transmissions;
    entry->local_node = (strcmp(info->node_id, state->config.node_id) == 0);
    
    state->membership_count++;
    
    return entry;
}

/**
 * @brief Remove a node from the membership list
 * @note Caller must hold membership_lock
 */
void membership_remove(gossip_state_t* state, const char* node_id) {
    for (size_t i = 0; i < state->membership_count; i++) {
        if (strcmp(state->membership[i].info.node_id, node_id) == 0) {
            /* Shift remaining elements */
            if (i < state->membership_count - 1) {
                memmove(&state->membership[i],
                       &state->membership[i + 1],
                       (state->membership_count - i - 1) * sizeof(membership_entry_t));
            }
            state->membership_count--;
            return;
        }
    }
}

/**
 * @brief Update an existing node's information
 * @note Caller must hold membership_lock
 */
void membership_update(gossip_state_t* state, const gossip_node_info_t* info) {
    membership_entry_t* entry = membership_find(state, info->node_id);
    
    if (entry) {
        /* Update with higher incarnation or same incarnation but better status */
        bool should_update = false;
        
        if (info->incarnation > entry->info.incarnation) {
            should_update = true;
        } else if (info->incarnation == entry->info.incarnation) {
            /* ALIVE overrides SUSPECTED overrides FAILED */
            if (info->status == GOSSIP_NODE_ALIVE && entry->info.status != GOSSIP_NODE_ALIVE) {
                should_update = true;
            } else if (info->status == GOSSIP_NODE_SUSPECTED && entry->info.status == GOSSIP_NODE_FAILED) {
                should_update = true;
            }
        }
        
        if (should_update) {
            gossip_node_status_t old_status = entry->info.status;
            
            memcpy(&entry->info, info, sizeof(gossip_node_info_t));
            entry->info.last_seen_ms = get_time_ms();
            entry->transmit_count = state->config.max_transmissions;
            
            /* Update suspicion time if status changed to SUSPECTED */
            if (info->status == GOSSIP_NODE_SUSPECTED && old_status != GOSSIP_NODE_SUSPECTED) {
                entry->suspect_time_ms = get_time_ms();
            }
        }
    }
}

/* ============================================================================
 * STATE TRANSITION FUNCTIONS
 * ========================================================================= */

/**
 * @brief Mark a node as alive
 */
void gossip_mark_node_alive(gossip_state_t* state, const char* node_id) {
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    if (!entry) {
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    gossip_node_status_t old_status = entry->info.status;
    
    if (old_status != GOSSIP_NODE_ALIVE) {
        entry->info.status = GOSSIP_NODE_ALIVE;
        entry->info.last_seen_ms = get_time_ms();
        entry->transmit_count = state->config.max_transmissions;
        entry->suspect_time_ms = 0;
        
        /* Trigger recovery callback if node was previously failed */
        if (old_status == GOSSIP_NODE_FAILED && state->on_recovered) {
            gossip_node_info_t info_copy = entry->info;
            pthread_mutex_unlock(&state->membership_lock);
            
            state->on_recovered(&info_copy, state->on_recovered_data);
            
            /* Log recovery */
            if (state->config.logger) {
                logger_t* logger = (logger_t*)state->config.logger;
                LOG_INFO(logger, "gossip", "Node recovered",
                        "node_id", node_id, NULL);
            }
            
            return;
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
}

/**
 * @brief Mark a node as suspected
 */
void gossip_mark_node_suspected(gossip_state_t* state, const char* node_id) {
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    if (!entry) {
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    /* Don't suspect if node already failed or left */
    if (entry->info.status == GOSSIP_NODE_FAILED || entry->info.status == GOSSIP_NODE_LEFT) {
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    /* Don't suspect ourselves */
    if (entry->local_node) {
        /* Refute suspicion by incrementing incarnation */
        state->self_incarnation++;
        entry->info.incarnation = state->self_incarnation;
        entry->info.status = GOSSIP_NODE_ALIVE;
        entry->transmit_count = state->config.max_transmissions;
        
        pthread_mutex_unlock(&state->membership_lock);
        
        if (state->config.logger) {
            logger_t* logger = (logger_t*)state->config.logger;
            LOG_WARN(logger, "gossip", "Refuted suspicion of self",
                    "new_incarnation", NULL, NULL);
        }
        
        return;
    }
    
    if (entry->info.status != GOSSIP_NODE_SUSPECTED) {
        entry->info.status = GOSSIP_NODE_SUSPECTED;
        entry->suspect_time_ms = get_time_ms();
        entry->transmit_count = state->config.max_transmissions;
        
        gossip_node_info_t info_copy = entry->info;
        pthread_mutex_unlock(&state->membership_lock);
        
        /* Trigger suspicion callback */
        if (state->on_suspected) {
            state->on_suspected(&info_copy, state->on_suspected_data);
        }
        
        /* Update metrics */
        if (state->suspicions_metric) {
            metrics_counter_inc((metrics_counter_t*)state->suspicions_metric);
        }
        
        /* Log suspicion */
        if (state->config.logger) {
            logger_t* logger = (logger_t*)state->config.logger;
            LOG_WARN(logger, "gossip", "Node suspected",
                    "node_id", node_id, NULL);
        }
    } else {
        pthread_mutex_unlock(&state->membership_lock);
    }
}

/**
 * @brief Mark a node as failed
 */
void gossip_mark_node_failed(gossip_state_t* state, const char* node_id) {
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    if (!entry) {
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    /* Don't fail ourselves */
    if (entry->local_node) {
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    if (entry->info.status != GOSSIP_NODE_FAILED) {
        entry->info.status = GOSSIP_NODE_FAILED;
        entry->transmit_count = state->config.max_transmissions;
        
        gossip_node_info_t info_copy = entry->info;
        pthread_mutex_unlock(&state->membership_lock);
        
        /* Trigger failure callback */
        if (state->on_failed) {
            state->on_failed(&info_copy, state->on_failed_data);
        }
        
        /* Update metrics */
        if (state->failures_metric) {
            metrics_counter_inc((metrics_counter_t*)state->failures_metric);
        }
        
        /* Log failure */
        if (state->config.logger) {
            logger_t* logger = (logger_t*)state->config.logger;
            LOG_ERROR(logger, "gossip", "Node failed",
                     "node_id", node_id, NULL);
        }
    } else {
        pthread_mutex_unlock(&state->membership_lock);
    }
}

/**
 * @brief Handle a node gracefully leaving
 */
void gossip_handle_node_leave(gossip_state_t* state, const char* node_id) {
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    if (!entry) {
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    entry->info.status = GOSSIP_NODE_LEFT;
    entry->transmit_count = state->config.max_transmissions;
    
    pthread_mutex_unlock(&state->membership_lock);
    
    /* Log leave */
    if (state->config.logger) {
        logger_t* logger = (logger_t*)state->config.logger;
        LOG_INFO(logger, "gossip", "Node left gracefully",
                "node_id", node_id, NULL);
    }
}

/* ============================================================================
 * SUSPICION TIMEOUT CHECKING
 * ========================================================================= */

/**
 * @brief Check if suspected nodes should transition to failed
 */
void gossip_check_suspicion_timeouts(gossip_state_t* state) {
    uint64_t now = get_time_ms();
    
    /* Calculate suspicion timeout (protocol_period * suspicion_mult) */
    uint64_t timeout_ms = state->config.protocol_period_ms * state->config.suspicion_mult;
    
    pthread_mutex_lock(&state->membership_lock);
    
    for (size_t i = 0; i < state->membership_count; i++) {
        membership_entry_t* entry = &state->membership[i];
        
        if (entry->info.status == GOSSIP_NODE_SUSPECTED) {
            uint64_t suspect_duration = now - entry->suspect_time_ms;
            
            if (suspect_duration >= timeout_ms) {
                /* Transition to failed */
                char node_id_copy[64];
                strncpy(node_id_copy, entry->info.node_id, sizeof(node_id_copy) - 1);
                node_id_copy[sizeof(node_id_copy) - 1] = '\0';
                
                pthread_mutex_unlock(&state->membership_lock);
                
                gossip_mark_node_failed(state, node_id_copy);
                
                pthread_mutex_lock(&state->membership_lock);
                
                /* Note: membership may have been modified, restart iteration */
                i = 0;
                if (state->membership_count == 0) break;
            }
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
}

/* ============================================================================
 * UPDATE DISSEMINATION
 * ========================================================================= */

/**
 * @brief Get updates to piggyback on outgoing messages
 */
void gossip_get_updates_to_broadcast(
    gossip_state_t* state,
    gossip_node_info_t** updates_out,
    uint32_t* count_out
) {
    pthread_mutex_lock(&state->membership_lock);
    
    /* Count nodes with transmit_count > 0 */
    size_t broadcast_count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].transmit_count > 0) {
            broadcast_count++;
        }
    }
    
    /* Limit to max 10 updates per message */
    if (broadcast_count > 10) {
        broadcast_count = 10;
    }
    
    if (broadcast_count == 0) {
        *updates_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    /* Allocate updates array */
    gossip_node_info_t* updates = (gossip_node_info_t*)malloc(
        broadcast_count * sizeof(gossip_node_info_t)
    );
    
    if (!updates) {
        *updates_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&state->membership_lock);
        return;
    }
    
    /* Collect updates and decrement transmit counts */
    size_t update_idx = 0;
    for (size_t i = 0; i < state->membership_count && update_idx < broadcast_count; i++) {
        if (state->membership[i].transmit_count > 0) {
            memcpy(&updates[update_idx], &state->membership[i].info, sizeof(gossip_node_info_t));
            state->membership[i].transmit_count--;
            update_idx++;
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
    
    *updates_out = updates;
    *count_out = (uint32_t)update_idx;
}

/**
 * @brief Process updates from a received message
 */
void gossip_update_node_from_message(
    gossip_state_t* state,
    const struct gossip_message_s* msg,
    const char* src_addr,
    uint16_t src_port
) {
    /* First, update the sender's information */
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* sender = membership_find(state, msg->sender_id);
    
    if (!sender) {
        /* New node - add it */
        gossip_node_info_t new_node;
        memset(&new_node, 0, sizeof(new_node));
        
        strncpy(new_node.node_id, msg->sender_id, sizeof(new_node.node_id) - 1);
        strncpy(new_node.address, src_addr, sizeof(new_node.address) - 1);
        new_node.port = src_port;
        new_node.status = GOSSIP_NODE_ALIVE;
        new_node.incarnation = msg->incarnation;
        new_node.role = GOSSIP_ROLE_WORKER;  /* Default to worker */
        
        sender = membership_add(state, &new_node);
        
        if (sender) {
            gossip_node_info_t info_copy = sender->info;
            pthread_mutex_unlock(&state->membership_lock);
            
            /* Trigger joined callback */
            if (state->on_joined) {
                state->on_joined(&info_copy, state->on_joined_data);
            }
            
            /* Log join */
            if (state->config.logger) {
                logger_t* logger = (logger_t*)state->config.logger;
                LOG_INFO(logger, "gossip", "Node joined",
                        "node_id", msg->sender_id,
                        "address", src_addr, NULL);
            }
            
            pthread_mutex_lock(&state->membership_lock);
        }
    } else {
        /* Update existing node */
        if (msg->incarnation >= sender->info.incarnation) {
            sender->info.incarnation = msg->incarnation;
            sender->info.last_seen_ms = get_time_ms();
            
            /* If node was suspected or failed, mark as alive if incarnation increased */
            if (msg->incarnation > sender->info.incarnation) {
                sender->info.status = GOSSIP_NODE_ALIVE;
                sender->suspect_time_ms = 0;
            }
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
    
    /* Process piggybacked updates */
    for (uint32_t i = 0; i < msg->update_count; i++) {
        const gossip_node_info_t* update = &msg->updates[i];
        
        /* Skip updates about ourselves */
        if (strcmp(update->node_id, state->config.node_id) == 0) {
            /* If someone thinks we're suspected/failed, refute it */
            if (update->status != GOSSIP_NODE_ALIVE) {
                state->self_incarnation++;
                
                pthread_mutex_lock(&state->membership_lock);
                membership_entry_t* self = membership_find(state, state->config.node_id);
                if (self) {
                    self->info.incarnation = state->self_incarnation;
                    self->info.status = GOSSIP_NODE_ALIVE;
                    self->transmit_count = state->config.max_transmissions;
                }
                pthread_mutex_unlock(&state->membership_lock);
            }
            continue;
        }
        
        /* Apply update */
        pthread_mutex_lock(&state->membership_lock);
        
        membership_entry_t* entry = membership_find(state, update->node_id);
        
        if (!entry) {
            /* New node from gossip */
            membership_add(state, update);
        } else {
            /* Update existing */
            membership_update(state, update);
        }
        
        pthread_mutex_unlock(&state->membership_lock);
    }
}