/**
 * @file raft_core.c
 * @brief Raft Consensus Core Implementation
 * 
 * Implements leader election, log replication, and safety guarantees.
 * 
 * Integrates:
 * - Session 3.1: Leader election foundation
 * - Session 3.2: RPC integration helpers
 * - Session 3.3: Log replication mechanism
 * - Session 3.4: Persistence layer
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_raft/raft_core.h"
#include "distric_raft/raft_replication.h"
#include "distric_raft/raft_persistence.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/**
 * @brief Raft log (in-memory)
 */
typedef struct {
    raft_log_entry_t* entries;  /**< Array of log entries */
    size_t capacity;            /**< Allocated capacity */
    size_t count;               /**< Number of entries */
    uint32_t base_index;        /**< Index of first entry (after snapshot) */
    uint32_t base_term;         /**< Term of last snapshot */
} raft_log_t;

/**
 * @brief Raft node internal structure
 */
struct raft_node {
    /* Configuration */
    raft_config_t config;
    
    /* Persistent state (must be persisted before responding to RPCs) */
    uint32_t current_term;          /**< Latest term seen */
    char voted_for[64];             /**< Candidate that received vote in current term */
    raft_log_t log;                 /**< Log entries */
    
    /* Volatile state (all servers) */
    _Atomic uint32_t commit_index;  /**< Highest log entry known to be committed */
    uint32_t last_applied;          /**< Highest log entry applied to state machine */
    
    /* Volatile state (leaders) */
    uint32_t* next_index;           /**< For each peer: next log entry to send */
    uint32_t* match_index;          /**< For each peer: highest log entry replicated */
    
    /* Node state */
    raft_state_t state;
    char current_leader[64];        /**< Current leader ID (if known) */
    
    /* Timing */
    uint64_t election_timeout_ms;   /**< Current election timeout */
    uint64_t last_heartbeat_ms;     /**< Last time we received heartbeat */
    uint64_t last_election_ms;      /**< Last time we started election */
    uint64_t last_heartbeat_sent_ms; /**< Last time we sent heartbeat (leader only) */
    
    /* Persistence (Session 3.4) */
    raft_persistence_t* persistence;
    
    /* Metrics */
    metric_t* state_metric;
    metric_t* term_metric;
    metric_t* log_size_metric;
    metric_t* commit_index_metric;
    metric_t* leader_changes_metric;
    
    /* Thread safety */
    pthread_rwlock_t lock;
    
    /* Control */
    _Atomic bool running;
};

/* ============================================================================
 * TIMING UTILITIES
 * ========================================================================= */

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static uint64_t random_election_timeout(uint32_t min_ms, uint32_t max_ms) {
    uint32_t range = max_ms - min_ms;
    return min_ms + (rand() % range);
}

/* ============================================================================
 * PERSISTENCE HELPERS (Session 3.4)
 * ========================================================================= */

/**
 * @brief Persist current state to disk
 */
static void persist_state(raft_node_t* node) {
    if (!node->persistence) {
        return;
    }
    
    distric_err_t err = raft_persistence_save_state(node->persistence, 
                                                     node->current_term, 
                                                     node->voted_for);
    if (err != DISTRIC_OK) {
        LOG_ERROR(node->config.logger, "raft", "Failed to persist state",
                 "term", &(int){node->current_term});
    }
}

/**
 * @brief Persist log entry to disk
 */
static distric_err_t persist_entry(raft_node_t* node, const raft_log_entry_t* entry) {
    if (!node->persistence) {
        return DISTRIC_OK;
    }
    
    return raft_persistence_append_log(node->persistence, entry);
}

/* ============================================================================
 * LOG OPERATIONS
 * ========================================================================= */

static distric_err_t raft_log_init(raft_log_t* log) {
    memset(log, 0, sizeof(raft_log_t));
    
    log->capacity = 1024;
    log->entries = (raft_log_entry_t*)calloc(log->capacity, sizeof(raft_log_entry_t));
    if (!log->entries) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    log->count = 0;
    log->base_index = 0;
    log->base_term = 0;
    
    return DISTRIC_OK;
}

static void raft_log_destroy(raft_log_t* log) {
    if (!log) return;
    
    for (size_t i = 0; i < log->count; i++) {
        free(log->entries[i].data);
    }
    
    free(log->entries);
    memset(log, 0, sizeof(raft_log_t));
}

static distric_err_t log_append(raft_log_t* log, uint32_t term, raft_entry_type_t type,
                                 const uint8_t* data, size_t data_len,
                                 raft_node_t* node) {
    /* Grow if needed */
    if (log->count >= log->capacity) {
        size_t new_capacity = log->capacity * 2;
        raft_log_entry_t* new_entries = (raft_log_entry_t*)realloc(
            log->entries, new_capacity * sizeof(raft_log_entry_t));
        if (!new_entries) {
            return DISTRIC_ERR_NO_MEMORY;
        }
        log->entries = new_entries;
        log->capacity = new_capacity;
    }
    
    /* Create entry */
    raft_log_entry_t* entry = &log->entries[log->count];
    entry->index = log->base_index + log->count + 1;
    entry->term = term;
    entry->type = type;
    
    /* Copy data */
    if (data && data_len > 0) {
        entry->data = (uint8_t*)malloc(data_len);
        if (!entry->data) {
            return DISTRIC_ERR_NO_MEMORY;
        }
        memcpy(entry->data, data, data_len);
        entry->data_len = data_len;
    } else {
        entry->data = NULL;
        entry->data_len = 0;
    }
    
    log->count++;
    
    /* Persist to disk (Session 3.4) */
    if (node && node->persistence) {
        distric_err_t err = persist_entry(node, entry);
        if (err != DISTRIC_OK) {
            LOG_ERROR(node->config.logger, "raft", "Failed to persist log entry",
                     "index", &(int){entry->index});
            /* Rollback in-memory append */
            free(entry->data);
            log->count--;
            return err;
        }
    }
    
    return DISTRIC_OK;
}

static raft_log_entry_t* log_get(const raft_log_t* log, uint32_t index) {
    if (index <= log->base_index) {
        return NULL;  /* In snapshot */
    }
    
    uint32_t offset = index - log->base_index - 1;
    if (offset >= log->count) {
        return NULL;  /* Beyond log */
    }
    
    return &log->entries[offset];
}

static uint32_t log_last_index(const raft_log_t* log) {
    if (log->count == 0) {
        return log->base_index;
    }
    return log->base_index + log->count;
}

static uint32_t log_last_term(const raft_log_t* log) {
    if (log->count == 0) {
        return log->base_term;
    }
    return log->entries[log->count - 1].term;
}

static distric_err_t log_truncate(raft_log_t* log, uint32_t from_index, raft_node_t* node) {
    if (from_index <= log->base_index) {
        return DISTRIC_OK;  /* Already in snapshot */
    }
    
    uint32_t offset = from_index - log->base_index - 1;
    if (offset >= log->count) {
        return DISTRIC_OK;  /* Nothing to truncate */
    }
    
    /* Free truncated entries */
    for (size_t i = offset; i < log->count; i++) {
        free(log->entries[i].data);
    }
    
    log->count = offset;
    
    /* Persist truncation (Session 3.4) */
    if (node && node->persistence) {
        raft_persistence_truncate_log(node->persistence, from_index);
    }
    
    return DISTRIC_OK;
}

/* ============================================================================
 * STATE TRANSITIONS
 * ========================================================================= */

static void transition_to_follower(raft_node_t* node, uint32_t term) {
    raft_state_t old_state = node->state;
    
    node->current_term = term;
    node->state = RAFT_STATE_FOLLOWER;
    memset(node->voted_for, 0, sizeof(node->voted_for));
    memset(node->current_leader, 0, sizeof(node->current_leader));
    node->last_heartbeat_ms = get_time_ms();
    
    /* Persist state change (Session 3.4) */
    persist_state(node);
    
    if (old_state != RAFT_STATE_FOLLOWER) {
        LOG_INFO(node->config.logger, "raft", "Became FOLLOWER",
                "term", &(int){term}, "old_state", raft_state_to_string(old_state));
        
        if (node->config.state_change_fn) {
            node->config.state_change_fn(old_state, RAFT_STATE_FOLLOWER, node->config.user_data);
        }
    }
}

static void transition_to_candidate(raft_node_t* node) {
    raft_state_t old_state = node->state;
    
    node->current_term++;
    node->state = RAFT_STATE_CANDIDATE;
    strncpy(node->voted_for, node->config.node_id, sizeof(node->voted_for) - 1);
    memset(node->current_leader, 0, sizeof(node->current_leader));
    node->last_election_ms = get_time_ms();
    node->election_timeout_ms = random_election_timeout(
        node->config.election_timeout_min_ms,
        node->config.election_timeout_max_ms);
    
    /* Persist state change (Session 3.4) */
    persist_state(node);
    
    LOG_INFO(node->config.logger, "raft", "Became CANDIDATE",
            "term", &(int){node->current_term},
            "timeout_ms", &(int){node->election_timeout_ms});
    
    if (node->config.state_change_fn) {
        node->config.state_change_fn(old_state, RAFT_STATE_CANDIDATE, node->config.user_data);
    }
}

static void transition_to_leader(raft_node_t* node) {
    raft_state_t old_state = node->state;
    
    node->state = RAFT_STATE_LEADER;
    strncpy(node->current_leader, node->config.node_id, sizeof(node->current_leader) - 1);
    
    /* Initialize leader state (if we have peers) */
    uint32_t last_index = log_last_index(&node->log);
    if (node->config.peer_count > 0 && node->next_index && node->match_index) {
        for (size_t i = 0; i < node->config.peer_count; i++) {
            node->next_index[i] = last_index + 1;
            node->match_index[i] = 0;
        }
    }
    
    /* Append no-op entry to commit entries from previous terms */
    log_append(&node->log, node->current_term, RAFT_ENTRY_NOOP, NULL, 0, node);
    
    /* Initialize heartbeat timestamp */
    node->last_heartbeat_sent_ms = get_time_ms();
    
    LOG_INFO(node->config.logger, "raft", "Became LEADER",
            "term", &(int){node->current_term},
            "last_log_index", &(int){last_index});
    
    if (node->config.state_change_fn) {
        node->config.state_change_fn(old_state, RAFT_STATE_LEADER, node->config.user_data);
    }
    
    /* Update metrics */
    if (node->leader_changes_metric) {
        metrics_counter_inc(node->leader_changes_metric);
    }
}

/* ============================================================================
 * LOG REPLICATION HELPERS (Session 3.3)
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
 * - Majority of match_index[i] >= N (or single-node cluster)
 * - log[N].term == current_term
 */
static void update_commit_index(raft_node_t* node) {
    if (!node || node->state != RAFT_STATE_LEADER) {
        return;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    uint32_t old_commit = atomic_load(&node->commit_index);
    uint32_t last_log_index = log_last_index(&node->log);
    
    /* For single-node cluster, auto-commit everything */
    if (node->config.peer_count == 0) {
        if (last_log_index > old_commit) {
            atomic_store(&node->commit_index, last_log_index);
            
            LOG_INFO(node->config.logger, "raft", "Advanced commit index (single-node)",
                    "old", &(int){old_commit},
                    "new", &(int){last_log_index});
        }
        pthread_rwlock_unlock(&node->lock);
        return;
    }
    
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
 * @brief Get optimal batch size for log replication
 */
static size_t get_replication_batch_size(raft_node_t* node) {
    (void)node;
    return 100;  /* Default: 100 entries max */
}

/* ============================================================================
 * RAFT NODE LIFECYCLE
 * ========================================================================= */

distric_err_t raft_create(const raft_config_t* config, raft_node_t** node_out) {
    if (!config || !node_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Allow peer_count=0 for single-node clusters (testing/development) */
    
    raft_node_t* node = (raft_node_t*)calloc(1, sizeof(raft_node_t));
    if (!node) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    /* Copy configuration */
    memcpy(&node->config, config, sizeof(raft_config_t));
    
    /* Allocate peer arrays */
    if (config->peer_count > 0) {
        node->config.peers = (raft_peer_t*)calloc(config->peer_count, sizeof(raft_peer_t));
        if (!node->config.peers) {
            free(node);
            return DISTRIC_ERR_NO_MEMORY;
        }
        memcpy(node->config.peers, config->peers, config->peer_count * sizeof(raft_peer_t));
    } else {
        node->config.peers = NULL;
    }
    
    /* Initialize log */
    distric_err_t err = raft_log_init(&node->log);
    if (err != DISTRIC_OK) {
        free(node->config.peers);
        free(node);
        return err;
    }
    
    /* Initialize leader state arrays */
    if (config->peer_count > 0) {
        node->next_index = (uint32_t*)calloc(config->peer_count, sizeof(uint32_t));
        node->match_index = (uint32_t*)calloc(config->peer_count, sizeof(uint32_t));
        
        if (!node->next_index || !node->match_index) {
            raft_log_destroy(&node->log);
            free(node->config.peers);
            free(node->next_index);
            free(node->match_index);
            free(node);
            return DISTRIC_ERR_NO_MEMORY;
        }
    } else {
        node->next_index = NULL;
        node->match_index = NULL;
    }
    
    /* Initialize state */
    node->current_term = 0;
    node->state = RAFT_STATE_FOLLOWER;
    atomic_store(&node->commit_index, 0);
    node->last_applied = 0;
    node->last_heartbeat_ms = get_time_ms();
    node->last_heartbeat_sent_ms = get_time_ms();
    node->election_timeout_ms = random_election_timeout(
        config->election_timeout_min_ms,
        config->election_timeout_max_ms);
    
    /* Initialize lock */
    pthread_rwlock_init(&node->lock, NULL);
    
    /* Initialize persistence (Session 3.4) */
node->persistence = NULL;
if (config->persistence_data_dir) {
    raft_persistence_config_t persist_config = {
        .data_dir = config->persistence_data_dir,
        .logger = config->logger
    };
    
    err = raft_persistence_init(&persist_config, &node->persistence);
    if (err != DISTRIC_OK) {
        LOG_ERROR(config->logger, "raft", "Failed to initialize persistence",
                 "data_dir", config->persistence_data_dir);
        pthread_rwlock_destroy(&node->lock);
        raft_log_destroy(&node->log);
        free(node->config.peers);
        free(node->next_index);
        free(node->match_index);
        free(node);
        return err;
    }
    
    /* Load persistent state */
    uint32_t loaded_term = 0;
    char loaded_voted_for[64] = {0};
    
    err = raft_persistence_load_state(node->persistence, &loaded_term, loaded_voted_for);
    if (err == DISTRIC_OK) {
        node->current_term = loaded_term;
        strncpy(node->voted_for, loaded_voted_for, sizeof(node->voted_for) - 1);
        
        LOG_INFO(config->logger, "raft", "Loaded persistent state",
                "term", &(int){loaded_term},
                "voted_for", loaded_voted_for);
    } else if (err != DISTRIC_ERR_NOT_FOUND) {
        LOG_ERROR(config->logger, "raft", "Failed to load persistent state");
    }
    
    /* Load log entries */
    raft_log_entry_t* entries = NULL;
    size_t count = 0;
    err = raft_persistence_load_log(node->persistence, &entries, &count);
    if (err == DISTRIC_OK && count > 0) {
        /* Restore log from persistence */
        for (size_t i = 0; i < count; i++) {
            log_append(&node->log, entries[i].term, entries[i].type, 
                      entries[i].data, entries[i].data_len, NULL);  /* NULL = don't re-persist */
            free(entries[i].data);
        }
        free(entries);
        
        LOG_INFO(config->logger, "raft", "Loaded log from persistence",
                "entries", &(int){count});
    } else if (err != DISTRIC_ERR_NOT_FOUND && err != DISTRIC_OK) {
        LOG_ERROR(config->logger, "raft", "Failed to load log entries");
    }
}
    
    /* Register metrics */
    if (config->metrics) {
        metrics_register_gauge(config->metrics, "raft_state", "Current Raft state (0=follower, 1=candidate, 2=leader)",
                              NULL, 0, &node->state_metric);
        metrics_register_gauge(config->metrics, "raft_term", "Current Raft term",
                              NULL, 0, &node->term_metric);
        metrics_register_gauge(config->metrics, "raft_log_size", "Number of log entries",
                              NULL, 0, &node->log_size_metric);
        metrics_register_gauge(config->metrics, "raft_commit_index", "Committed log index",
                              NULL, 0, &node->commit_index_metric);
        metrics_register_counter(config->metrics, "raft_leader_changes_total", "Total leader changes",
                                 NULL, 0, &node->leader_changes_metric);
    }
    
    *node_out = node;
    
    LOG_INFO(config->logger, "raft", "Node created",
            "node_id", config->node_id,
            "peers", &(int){config->peer_count},
            "persistence", config->persistence_data_dir ? "enabled" : "disabled");
    
    return DISTRIC_OK;
}

void raft_destroy(raft_node_t* node) {
    if (!node) return;
    
    atomic_store(&node->running, false);
    
    pthread_rwlock_destroy(&node->lock);
    
    raft_log_destroy(&node->log);
    
    /* Destroy persistence (Session 3.4) */
    if (node->persistence) {
        raft_persistence_destroy(node->persistence);
    }
    
    free(node->config.peers);
    free(node->next_index);
    free(node->match_index);
    
    free(node);
}

distric_err_t raft_start(raft_node_t* node) {
    if (!node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    atomic_store(&node->running, true);
    
    LOG_INFO(node->config.logger, "raft", "Node started");
    
    return DISTRIC_OK;
}

distric_err_t raft_stop(raft_node_t* node) {
    if (!node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    atomic_store(&node->running, false);
    
    LOG_INFO(node->config.logger, "raft", "Node stopped");
    
    return DISTRIC_OK;
}

/* ============================================================================
 * ELECTION LOGIC
 * ========================================================================= */

static void start_election(raft_node_t* node) {
    pthread_rwlock_wrlock(&node->lock);
    
    transition_to_candidate(node);
    
    /* Vote for self */
    int votes_needed = (node->config.peer_count + 1) / 2 + 1;
    
    uint32_t last_log_index = log_last_index(&node->log);
    uint32_t last_log_term = log_last_term(&node->log);
    
    pthread_rwlock_unlock(&node->lock);
    
    LOG_INFO(node->config.logger, "raft", "Starting election",
            "term", &(int){node->current_term},
            "votes_needed", &votes_needed,
            "last_log_index", &(int){last_log_index},
            "last_log_term", &(int){last_log_term});
    
    /* If cluster size is 1, immediately become leader */
    if (votes_needed == 1) {
        pthread_rwlock_wrlock(&node->lock);
        transition_to_leader(node);
        pthread_rwlock_unlock(&node->lock);
    }
}

/* ============================================================================
 * TICK (Main Event Loop Driver)
 * ========================================================================= */

distric_err_t raft_tick(raft_node_t* node) {
    if (!node || !atomic_load(&node->running)) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&node->lock);
    
    uint64_t now = get_time_ms();
    raft_state_t state = node->state;
    
    /* Update metrics */
    if (node->state_metric) {
        metrics_gauge_set(node->state_metric, (double)node->state);
    }
    if (node->term_metric) {
        metrics_gauge_set(node->term_metric, (double)node->current_term);
    }
    if (node->log_size_metric) {
        metrics_gauge_set(node->log_size_metric, (double)node->log.count);
    }
    if (node->commit_index_metric) {
        metrics_gauge_set(node->commit_index_metric, (double)atomic_load(&node->commit_index));
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    /* State-specific logic */
    switch (state) {
        case RAFT_STATE_FOLLOWER:
        case RAFT_STATE_CANDIDATE: {
            /* Check election timeout */
            pthread_rwlock_rdlock(&node->lock);
            uint64_t elapsed = now - node->last_heartbeat_ms;
            uint64_t timeout = node->election_timeout_ms;
            pthread_rwlock_unlock(&node->lock);
            
            if (elapsed > timeout) {
                start_election(node);
            }
            
            /* Apply committed entries (follower) */
            apply_committed_entries(node);
            
            break;
        }
        
        case RAFT_STATE_LEADER: {
            /* Apply committed entries (leader) */
            apply_committed_entries(node);
            
            /* Update commit index based on match_index */
            update_commit_index(node);
            
            break;
        }
    }
    
    return DISTRIC_OK;
}

/* ============================================================================
 * RPC HANDLERS
 * ========================================================================= */

distric_err_t raft_handle_request_vote(
    raft_node_t* node,
    const char* candidate_id,
    uint32_t term,
    uint32_t last_log_index,
    uint32_t last_log_term,
    bool* vote_granted_out,
    uint32_t* term_out
) {
    if (!node || !candidate_id || !vote_granted_out || !term_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    *vote_granted_out = false;
    
    /* If RPC term > current term, update and become follower */
    if (term > node->current_term) {
        transition_to_follower(node, term);
    }
    
    /* Set term_out AFTER potential update */
    *term_out = node->current_term;
    
    /* Reject if term < current term */
    if (term < node->current_term) {
        pthread_rwlock_unlock(&node->lock);
        return DISTRIC_OK;
    }
    
    /* Check if we can vote for this candidate */
    bool can_vote = (node->voted_for[0] == '\0' || 
                    strcmp(node->voted_for, candidate_id) == 0);
    
    if (!can_vote) {
        pthread_rwlock_unlock(&node->lock);
        return DISTRIC_OK;
    }
    
    /* Check if candidate's log is at least as up-to-date as ours */
    uint32_t our_last_index = log_last_index(&node->log);
    uint32_t our_last_term = log_last_term(&node->log);
    
    bool log_ok = (last_log_term > our_last_term) ||
                  (last_log_term == our_last_term && last_log_index >= our_last_index);
    
    if (log_ok) {
        *vote_granted_out = true;
        strncpy(node->voted_for, candidate_id, sizeof(node->voted_for) - 1);
        node->last_heartbeat_ms = get_time_ms();  /* Reset election timer */
        
        /* Persist state BEFORE responding (Session 3.4) */
        persist_state(node);
        
        LOG_INFO(node->config.logger, "raft", "Granted vote",
                "candidate", candidate_id,
                "term", &(int){term});
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

distric_err_t raft_handle_append_entries(
    raft_node_t* node,
    const char* leader_id,
    uint32_t term,
    uint32_t prev_log_index,
    uint32_t prev_log_term,
    const raft_log_entry_t* entries,
    size_t entry_count,
    uint32_t leader_commit,
    bool* success_out,
    uint32_t* term_out,
    uint32_t* last_log_index_out
) {
    if (!node || !leader_id || !success_out || !term_out || !last_log_index_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    *success_out = false;
    
    /* If RPC term > current term, update and become follower */
    if (term > node->current_term) {
        transition_to_follower(node, term);
    }
    
    /* Set term_out AFTER potential update */
    *term_out = node->current_term;
    *last_log_index_out = log_last_index(&node->log);
    
    /* Reject if term < current term */
    if (term < node->current_term) {
        pthread_rwlock_unlock(&node->lock);
        return DISTRIC_OK;
    }
    
    /* Valid leader - reset election timer */
    node->last_heartbeat_ms = get_time_ms();
    strncpy(node->current_leader, leader_id, sizeof(node->current_leader) - 1);
    
    /* If we were candidate, become follower */
    if (node->state == RAFT_STATE_CANDIDATE) {
        transition_to_follower(node, term);
    }
    
    /* Check if log contains entry at prev_log_index with matching term */
    if (prev_log_index > 0) {
        raft_log_entry_t* prev_entry = log_get(&node->log, prev_log_index);
        
        if (!prev_entry || prev_entry->term != prev_log_term) {
            /* Log doesn't match - reject */
            pthread_rwlock_unlock(&node->lock);
            return DISTRIC_OK;
        }
    }
    
    /* Append new entries */
    if (entries && entry_count > 0) {
        for (size_t i = 0; i < entry_count; i++) {
            const raft_log_entry_t* entry = &entries[i];
            
            /* Check for conflicts */
            raft_log_entry_t* existing = log_get(&node->log, entry->index);
            if (existing && existing->term != entry->term) {
                /* Delete conflicting entry and all that follow */
                log_truncate(&node->log, entry->index, node);
            }
            
            /* Append entry if not already present */
            if (!log_get(&node->log, entry->index)) {
                log_append(&node->log, entry->term, entry->type, entry->data, entry->data_len, node);
            }
        }
    }
    
    /* Update commit index */
    if (leader_commit > atomic_load(&node->commit_index)) {
        uint32_t new_commit = leader_commit < log_last_index(&node->log) ? 
                             leader_commit : log_last_index(&node->log);
        atomic_store(&node->commit_index, new_commit);
    }
    
    *success_out = true;
    *last_log_index_out = log_last_index(&node->log);
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * LOG OPERATIONS (Public API)
 * ========================================================================= */

distric_err_t raft_append_entry(
    raft_node_t* node,
    const uint8_t* data,
    size_t data_len,
    uint32_t* index_out
) {
    if (!node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    if (node->state != RAFT_STATE_LEADER) {
        pthread_rwlock_unlock(&node->lock);
        return DISTRIC_ERR_NOT_FOUND;  /* Not leader */
    }
    
    distric_err_t err = log_append(&node->log, node->current_term, RAFT_ENTRY_COMMAND, data, data_len, node);
    if (err != DISTRIC_OK) {
        pthread_rwlock_unlock(&node->lock);
        return err;
    }
    
    uint32_t index = log_last_index(&node->log);
    if (index_out) {
        *index_out = index;
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

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
 * STATE QUERIES
 * ========================================================================= */

raft_state_t raft_get_state(const raft_node_t* node) {
    if (!node) return RAFT_STATE_FOLLOWER;
    return node->state;
}

bool raft_is_leader(const raft_node_t* node) {
    return node && node->state == RAFT_STATE_LEADER;
}

uint32_t raft_get_term(const raft_node_t* node) {
    return node ? node->current_term : 0;
}

distric_err_t raft_get_leader(const raft_node_t* node, char* leader_id_out) {
    if (!node || !leader_id_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&node->lock);
    
    if (node->current_leader[0] == '\0') {
        pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    strncpy(leader_id_out, node->current_leader, 64);
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
    
    return DISTRIC_OK;
}

uint32_t raft_get_commit_index(const raft_node_t* node) {
    return node ? atomic_load((const _Atomic uint32_t*)&node->commit_index) : 0;
}

uint32_t raft_get_last_log_index(const raft_node_t* node) {
    if (!node) return 0;
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&node->lock);
    uint32_t index = log_last_index(&node->log);
    pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
    
    return index;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* raft_state_to_string(raft_state_t state) {
    switch (state) {
        case RAFT_STATE_FOLLOWER:  return "FOLLOWER";
        case RAFT_STATE_CANDIDATE: return "CANDIDATE";
        case RAFT_STATE_LEADER:    return "LEADER";
        default:                   return "UNKNOWN";
    }
}

/* ============================================================================
 * RPC HELPER FUNCTIONS (Session 3.2)
 * ========================================================================= */

const raft_config_t* raft_get_config(const raft_node_t* node) {
    return node ? &node->config : NULL;
}

const char* raft_get_node_id(const raft_node_t* node) {
    return node ? node->config.node_id : "";
}

uint32_t raft_get_last_log_term(const raft_node_t* node) {
    if (!node) return 0;
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&node->lock);
    uint32_t term = log_last_term(&node->log);
    pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
    
    return term;
}

uint32_t raft_get_log_term(const raft_node_t* node, uint32_t index) {
    if (!node) return 0;
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&node->lock);
    
    raft_log_entry_t* entry = log_get(&node->log, index);
    uint32_t term = entry ? entry->term : 0;
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
    
    return term;
}

distric_err_t raft_get_log_entries(
    const raft_node_t* node,
    uint32_t start_index,
    uint32_t end_index,
    raft_log_entry_t** entries_out,
    size_t* count_out
) {
    if (!node || !entries_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&node->lock);
    
    size_t count = (end_index > start_index) ? (end_index - start_index) : 0;
    if (count == 0) {
        *entries_out = NULL;
        *count_out = 0;
        pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
        return DISTRIC_OK;
    }
    
    raft_log_entry_t* entries = (raft_log_entry_t*)calloc(count, sizeof(raft_log_entry_t));
    if (!entries) {
        pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    size_t actual_count = 0;
    for (uint32_t i = start_index; i < end_index; i++) {
        raft_log_entry_t* entry = log_get(&node->log, i);
        if (entry) {
            entries[actual_count] = *entry;
            
            /* Copy data */
            if (entry->data && entry->data_len > 0) {
                entries[actual_count].data = (uint8_t*)malloc(entry->data_len);
                if (entries[actual_count].data) {
                    memcpy(entries[actual_count].data, entry->data, entry->data_len);
                }
            }
            
            actual_count++;
        }
    }
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
    
    *entries_out = entries;
    *count_out = actual_count;
    
    return DISTRIC_OK;
}

uint32_t raft_get_peer_next_index(const raft_node_t* node, size_t peer_index) {
    if (!node || peer_index >= node->config.peer_count) {
        return 0;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&node->lock);
    uint32_t next_index = node->next_index[peer_index];
    pthread_rwlock_unlock((pthread_rwlock_t*)&node->lock);
    
    return next_index;
}

distric_err_t raft_update_peer_indices(
    raft_node_t* node,
    size_t peer_index,
    uint32_t next_index,
    uint32_t match_index
) {
    if (!node || peer_index >= node->config.peer_count) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    node->next_index[peer_index] = next_index;
    node->match_index[peer_index] = match_index;
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

distric_err_t raft_decrement_peer_next_index(raft_node_t* node, size_t peer_index) {
    if (!node || peer_index >= node->config.peer_count) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    if (node->next_index[peer_index] > 1) {
        node->next_index[peer_index]--;
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

distric_err_t raft_step_down(raft_node_t* node, uint32_t term) {
    if (!node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    if (term > node->current_term) {
        transition_to_follower(node, term);
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * REPLICATION API (Session 3.3)
 * ========================================================================= */

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
    
    return (next_index <= last_log_index);
}

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

void raft_mark_heartbeat_sent(raft_node_t* node) {
    if (!node) return;
    
    pthread_rwlock_wrlock(&node->lock);
    node->last_heartbeat_sent_ms = get_time_ms();
    pthread_rwlock_unlock(&node->lock);
}

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

void raft_free_log_entries(raft_log_entry_t* entries, size_t count) {
    if (!entries) return;
    
    for (size_t i = 0; i < count; i++) {
        free(entries[i].data);
    }
    
    free(entries);
}

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
            if (i == 1) break;
        }
    } else {
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

/* ============================================================================
 * SNAPSHOT API (Session 3.4 Integration)
 * ========================================================================= */

distric_err_t raft_create_snapshot(
    raft_node_t* node,
    const uint8_t* snapshot_data,
    size_t snapshot_len
) {
    if (!node || !snapshot_data) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!node->persistence) {
        return DISTRIC_ERR_INVALID_ARG;  /* Persistence not enabled */
    }
    
    pthread_rwlock_wrlock(&node->lock);
    
    /* Use commit_index instead of last_applied for snapshot point */
    uint32_t last_included_index = atomic_load(&node->commit_index);
    uint32_t last_included_term;
    
    /* Handle case where nothing has been committed yet */
    if (last_included_index == 0) {
        /* Use base term or term 0 */
        last_included_term = node->log.base_term;
        
        /* If base_term is also 0, use current_term */
        if (last_included_term == 0) {
            last_included_term = node->current_term;
        }
    } else {
        /* Get the term of the last committed entry */
        raft_log_entry_t* entry = log_get(&node->log, last_included_index);
        
        if (entry) {
            last_included_term = entry->term;
        } else {
            /* Entry might be in a previous snapshot */
            last_included_term = node->log.base_term;
        }
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    /* Save snapshot to persistence */
    distric_err_t err = raft_persistence_save_snapshot(node->persistence, 
                                                        last_included_index,
                                                        last_included_term,
                                                        snapshot_data, 
                                                        snapshot_len);
    if (err != DISTRIC_OK) {
        LOG_ERROR(node->config.logger, "raft", "Failed to save snapshot");
        return err;
    }
    
    /* Update base index/term */
    pthread_rwlock_wrlock(&node->lock);
    node->log.base_index = last_included_index;
    node->log.base_term = last_included_term;
    
    /* Update last_applied if needed */
    if (last_included_index > node->last_applied) {
        node->last_applied = last_included_index;
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    LOG_INFO(node->config.logger, "raft", "Snapshot created",
            "last_included_index", &(int){last_included_index},
            "last_included_term", &(int){last_included_term},
            "data_len", &(int){snapshot_len});
    
    return DISTRIC_OK;
}

distric_err_t raft_install_snapshot(
    raft_node_t* node,
    uint32_t last_included_index,
    uint32_t last_included_term,
    const uint8_t* snapshot_data,
    size_t snapshot_len
) {
    if (!node || !snapshot_data) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!node->persistence) {
        return DISTRIC_ERR_INVALID_ARG;  /* Persistence not enabled */
    }
    
    /* Save snapshot to persistence */
    distric_err_t err = raft_persistence_save_snapshot(node->persistence,
                                                        last_included_index,
                                                        last_included_term,
                                                        snapshot_data,
                                                        snapshot_len);
    if (err != DISTRIC_OK) {
        LOG_WARN(node->config.logger, "raft", "Snapshot save not implemented yet");
        return err;
    }
    
    /* Update state */
    pthread_rwlock_wrlock(&node->lock);
    
    node->log.base_index = last_included_index;
    node->log.base_term = last_included_term;
    
    /* Discard log entries covered by snapshot */
    log_truncate(&node->log, last_included_index + 1, NULL);  /* NULL = already persisted */
    
    /* Update commit and applied indices */
    if (last_included_index > atomic_load(&node->commit_index)) {
        atomic_store(&node->commit_index, last_included_index);
    }
    if (last_included_index > node->last_applied) {
        node->last_applied = last_included_index;
    }
    
    pthread_rwlock_unlock(&node->lock);
    
    LOG_INFO(node->config.logger, "raft", "Snapshot installed",
            "last_included_index", &(int){last_included_index},
            "last_included_term", &(int){last_included_term});
    
    return DISTRIC_OK;
}