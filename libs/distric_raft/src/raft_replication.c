/**
 * @file raft_replication.c
 * @brief Raft Log Replication Implementation
 * 
 * Implements leader-driven log replication with:
 * - Heartbeat mechanism
 * - Log consistency checks
 * - Commit index advancement
 * - Conflict resolution
 * 
 * This code integrates into raft_core.c
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* Forward declarations - these exist in raft_core.c */
typedef struct raft_node raft_node_t;
typedef struct raft_log_s raft_log_t;

/* ============================================================================
 * LOG REPLICATION HELPERS
 * ========================================================================= */

/**
 * @brief Apply committed entries to state machine
 * 
 * Applies all entries from last_applied+1 to commit_index.
 * This is called by both leaders and followers.
 */
static void apply_committed_entries(raft_node_t* node) {
    if (!node || !node->config.apply_fn) {
        return;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    uint32_t commit_index = atomic_load(&node->commit_index);
    
    while (node->last_applied < commit_index) {
        node->last_applied++;
        
        /* Get entry */
        raft_log_entry_t* entry = log_get(&node->log, node->last_applied);
        if (!entry) {
            LOG_ERROR(node->config.logger, "raft", "Missing log entry",
                     "index", &(int){node->last_applied});
            break;
        }
        
        /* Skip NO-OP entries */
        if (entry->type == RAFT_ENTRY_NOOP) {
            LOG_DEBUG(node->config.logger, "raft", "Skipping NO-OP entry",
                     "index", &(int){node->last_applied});
            continue;
        }
        
        /* Apply to state machine */
        pthread_rwlock_unlock(&node->lock);
        node->config.apply_fn(entry, node->config.user_data);
        pthread_rwlock_wrlock(&node->lock);
        
        LOG_DEBUG(node->config.logger, "raft", "Applied entry to state machine",
                 "index", &(int){node->last_applied},
                 "term", &(int){entry->term});
    }
    
    pthread_rwlock_unlock(&node->lock);
}

/**
 * @brief Update commit index based on match_index
 * 
 * Leader advances commit_index to highest N where:
 * - N > commit_index
 * - Majority of match_index[i] >= N
 * - log[N].term == current_term
 */
static void update_commit_index(raft_node_t* node) {
    if (!node || node->state != RAFT_STATE_LEADER) {
        return;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    uint32_t old_commit = atomic_load(&node->commit_index);
    uint32_t last_log_index = log_last_index(&node->log);
    
    /* Try each index from commit_index+1 to last_log_index */
    for (uint32_t n = old_commit + 1; n <= last_log_index; n++) {
        /* Check if entry is from current term */
        raft_log_entry_t* entry = log_get(&node->log, n);
        if (!entry || entry->term != node->current_term) {
            continue;
        }
        
        /* Count replicas (including self) */
        uint32_t replicas = 1;  /* Self */
        
        for (size_t i = 0; i < node->config.peer_count; i++) {
            if (node->match_index[i] >= n) {
                replicas++;
            }
        }
        
        /* Check if majority */
        uint32_t majority = (node->config.peer_count + 1) / 2 + 1;
        
        if (replicas >= majority) {
            atomic_store(&node->commit_index, n);
            
            LOG_INFO(node->config.logger, "raft", "Advanced commit index",
                    "old", &(int){old_commit},
                    "new", &(int){n},
                    "replicas", &(int){replicas},
                    "majority", &(int){majority});
            
            old_commit = n;
        }
    }
    
    pthread_rwlock_unlock(&node->lock);
}

/**
 * @brief Check if peer needs snapshot instead of log replication
 * 
 * Returns true if peer's next_index has been compacted into snapshot.
 */
static bool peer_needs_snapshot(raft_node_t* node, size_t peer_index) {
    pthread_rwlock_rdlock(&node->lock);
    
    uint32_t next_index = node->next_index[peer_index];
    uint32_t base_index = node->log.base_index;
    
    pthread_rwlock_unlock(&node->lock);
    
    return (next_index <= base_index);
}

/**
 * @brief Get optimal batch size for log replication
 * 
 * Limits number of entries sent in single AppendEntries to avoid
 * overloading network or peer.
 */
static size_t get_replication_batch_size(raft_node_t* node) {
    /* Configuration: max entries per AppendEntries RPC */
    (void)node;  /* May use config in future */
    return 100;  /* Default: 100 entries max */
}

/* ============================================================================
 * PUBLIC REPLICATION API
 * ========================================================================= */

/**
 * @brief Check if leader should replicate to peer
 * 
 * Returns true if peer has entries to replicate or needs heartbeat.
 */
bool raft_should_replicate_to_peer(raft_node_t* node, size_t peer_index) {
    if (!node || peer_index >= node->config.peer_count) {
        return false;
    }
    
    if (node->state != RAFT_STATE_LEADER) {
        return false;
    }
    
    pthread_rwlock_rdlock(&node->lock);
    
    uint32_t next_index = node->next_index[peer_index];
    uint32_t last_log_index = log_last_index(&node->log);
    
    pthread_rwlock_unlock(&node->lock);
    
    /* Replicate if peer is behind */
    return (next_index <= last_log_index);
}

/**
 * @brief Get heartbeat timeout
 * 
 * Returns true if it's time to send heartbeat.
 */
bool raft_should_send_heartbeat(raft_node_t* node) {
    if (!node || node->state != RAFT_STATE_LEADER) {
        return false;
    }
    
    pthread_rwlock_rdlock(&node->lock);
    
    uint64_t now = get_time_ms();
    uint64_t elapsed = now - node->last_heartbeat_sent_ms;
    uint64_t interval = node->config.heartbeat_interval_ms;
    
    pthread_rwlock_unlock(&node->lock);
    
    return (elapsed >= interval);
}

/**
 * @brief Mark heartbeat as sent
 */
void raft_mark_heartbeat_sent(raft_node_t* node) {
    if (!node) return;
    
    pthread_rwlock_wrlock(&node->lock);
    node->last_heartbeat_sent_ms = get_time_ms();
    pthread_rwlock_unlock(&node->lock);
}

/**
 * @brief Handle AppendEntries response from follower
 * 
 * Updates next_index and match_index based on response.
 * Called by RPC layer after receiving AppendEntries response.
 */
distric_err_t raft_handle_append_entries_response(
    raft_node_t* node,
    size_t peer_index,
    bool success,
    uint32_t peer_term,
    uint32_t match_index
) {
    if (!node || peer_index >= node->config.peer_count) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    /* Check if we've been deposed */
    if (peer_term > node->current_term) {
        pthread_rwlock_unlock(&node->lock);
        
        LOG_INFO(node->config.logger, "raft", "Stepping down due to higher term in AppendEntries response",
                "our_term", &(int){node->current_term},
                "peer_term", &(int){peer_term});
        
        return raft_step_down(node, peer_term);
    }
    
    /* Ignore stale responses */
    if (peer_term < node->current_term) {
        pthread_rwlock_unlock(&node->lock);
        return DISTRIC_OK;
    }
    
    if (success) {
        /* Update next_index and match_index */
        node->next_index[peer_index] = match_index + 1;
        node->match_index[peer_index] = match_index;
        
        LOG_DEBUG(node->config.logger, "raft", "Replication succeeded",
                 "peer_index", &(int){peer_index},
                 "match_index", &(int){match_index});
    } else {
        /* Decrement next_index and retry */
        if (node->next_index[peer_index] > 1) {
            node->next_index[peer_index]--;
        }
        
        LOG_DEBUG(node->config.logger, "raft", "Replication failed, decrementing next_index",
                 "peer_index", &(int){peer_index},
                 "next_index", &(int){node->next_index[peer_index]});
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    /* Update commit index if needed */
    update_commit_index(node);
    
    return DISTRIC_OK;
}

/**
 * @brief Wait for entry to be committed
 * 
 * Blocks until entry at given index is committed or timeout expires.
 */
distric_err_t raft_wait_committed(
    raft_node_t* node,
    uint32_t index,
    uint32_t timeout_ms
) {
    if (!node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    uint64_t start = get_time_ms();
    
    while (1) {
        uint32_t commit_index = atomic_load(&node->commit_index);
        
        if (commit_index >= index) {
            return DISTRIC_OK;
        }
        
        /* Check timeout */
        uint64_t elapsed = get_time_ms() - start;
        if (elapsed >= timeout_ms) {
            LOG_WARN(node->config.logger, "raft", "Wait for commit timed out",
                    "index", &(int){index},
                    "commit_index", &(int){commit_index},
                    "timeout_ms", &(int){timeout_ms});
            return DISTRIC_ERR_TIMEOUT;
        }
        
        /* Sleep briefly */
        usleep(10000);  /* 10ms */
    }
}

/* ============================================================================
 * OPTIMIZED BATCH REPLICATION
 * ========================================================================= */

/**
 * @brief Get entries to replicate to peer
 * 
 * Returns entries from next_index onwards, up to batch size limit.
 */
distric_err_t raft_get_entries_for_peer(
    raft_node_t* node,
    size_t peer_index,
    raft_log_entry_t** entries_out,
    size_t* count_out,
    uint32_t* prev_log_index_out,
    uint32_t* prev_log_term_out
) {
    if (!node || peer_index >= node->config.peer_count || 
        !entries_out || !count_out || !prev_log_index_out || !prev_log_term_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&node->lock);
    
    uint32_t next_index = node->next_index[peer_index];
    uint32_t last_log_index = log_last_index(&node->log);
    
    /* Calculate prev_log info */
    *prev_log_index_out = (next_index > 1) ? next_index - 1 : 0;
    *prev_log_term_out = 0;
    
    if (*prev_log_index_out > 0) {
        raft_log_entry_t* prev_entry = log_get(&node->log, *prev_log_index_out);
        if (prev_entry) {
            *prev_log_term_out = prev_entry->term;
        } else {
            /* prev_log_index is in snapshot */
            *prev_log_term_out = node->log.base_term;
        }
    }
    
    /* Calculate entries to send */
    if (next_index > last_log_index) {
        /* Peer is up-to-date, send empty AppendEntries (heartbeat) */
        *entries_out = NULL;
        *count_out = 0;
        pthread_rwlock_unlock(&node->lock);
        return DISTRIC_OK;
    }
    
    /* Limit batch size */
    size_t batch_size = get_replication_batch_size(node);
    uint32_t end_index = next_index + batch_size;
    if (end_index > last_log_index + 1) {
        end_index = last_log_index + 1;
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    /* Get entries */
    return raft_get_log_entries(node, next_index, end_index, entries_out, count_out);
}

/**
 * @brief Free entries returned by raft_get_entries_for_peer
 */
void raft_free_log_entries(raft_log_entry_t* entries, size_t count) {
    if (!entries) return;
    
    for (size_t i = 0; i < count; i++) {
        free(entries[i].data);
    }
    
    free(entries);
}

/* ============================================================================
 * CONFLICT RESOLUTION
 * ========================================================================= */

/**
 * @brief Handle log conflict with follower
 * 
 * When AppendEntries fails due to log inconsistency, this function
 * determines the optimal next_index to try.
 * 
 * Optimization: Instead of decrementing by 1, jump back to last
 * entry of conflicting term.
 */
distric_err_t raft_handle_log_conflict(
    raft_node_t* node,
    size_t peer_index,
    uint32_t conflict_index,
    uint32_t conflict_term
) {
    if (!node || peer_index >= node->config.peer_count) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    uint32_t new_next_index = 1;
    
    if (conflict_term > 0) {
        /* Find last entry of conflict_term */
        for (uint32_t i = conflict_index; i >= 1; i--) {
            raft_log_entry_t* entry = log_get(&node->log, i);
            if (entry && entry->term == conflict_term) {
                new_next_index = i + 1;
                break;
            }
            if (i == 1) break;  /* Avoid underflow */
        }
    } else {
        /* No conflict term info, just use conflict_index */
        new_next_index = conflict_index;
    }
    
    node->next_index[peer_index] = new_next_index;
    
    LOG_INFO(node->config.logger, "raft", "Handled log conflict",
            "peer_index", &(int){peer_index},
            "conflict_index", &(int){conflict_index},
            "conflict_term", &(int){conflict_term},
            "new_next_index", &(int){new_next_index});
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * METRICS AND MONITORING
 * ========================================================================= */

/**
 * @brief Get replication lag for peer
 * 
 * Returns number of entries peer is behind leader.
 */
uint32_t raft_get_peer_lag(raft_node_t* node, size_t peer_index) {
    if (!node || peer_index >= node->config.peer_count) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&node->lock);
    
    uint32_t last_log_index = log_last_index(&node->log);
    uint32_t match_index = node->match_index[peer_index];
    
    pthread_rwlock_unlock(&node->lock);
    
    return (last_log_index > match_index) ? (last_log_index - match_index) : 0;
}

/**
 * @brief Get replication progress summary
 * 
 * Returns statistics about replication progress.
 */
typedef struct {
    uint32_t last_log_index;
    uint32_t commit_index;
    uint32_t min_match_index;
    uint32_t max_match_index;
    uint32_t peers_up_to_date;
    uint32_t peers_lagging;
} replication_stats_t;

distric_err_t raft_get_replication_stats(
    raft_node_t* node,
    replication_stats_t* stats_out
) {
    if (!node || !stats_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (node->state != RAFT_STATE_LEADER) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&node->lock);
    
    stats_out->last_log_index = log_last_index(&node->log);
    stats_out->commit_index = atomic_load(&node->commit_index);
    stats_out->min_match_index = UINT32_MAX;
    stats_out->max_match_index = 0;
    stats_out->peers_up_to_date = 0;
    stats_out->peers_lagging = 0;
    
    for (size_t i = 0; i < node->config.peer_count; i++) {
        uint32_t match = node->match_index[i];
        
        if (match < stats_out->min_match_index) {
            stats_out->min_match_index = match;
        }
        if (match > stats_out->max_match_index) {
            stats_out->max_match_index = match;
        }
        
        if (match == stats_out->last_log_index) {
            stats_out->peers_up_to_date++;
        } else {
            stats_out->peers_lagging++;
        }
    }
    
    if (node->config.peer_count == 0) {
        stats_out->min_match_index = 0;
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}