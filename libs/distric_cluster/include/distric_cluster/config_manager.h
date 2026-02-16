/**
 * @file config_manager.h
 * @brief DistriC Configuration Manager - Raft-based Configuration Replication
 * 
 * This module manages cluster configuration replication using Raft consensus.
 * All configuration changes are replicated across coordinator nodes to ensure
 * consistency.
 * 
 * Replicated Configuration:
 * - Node roles (coordinator vs worker)
 * - System parameters (timeouts, thresholds)
 * - Future: Rule catalog and workflow definitions (integrated in later phases)
 * 
 * Thread Safety:
 * - All functions are thread-safe
 * - Reads use read-write lock (concurrent reads allowed)
 * - Writes require write lock and Raft consensus
 * 
 * @version 1.0.0
 * @author DistriC Development Team
 */

#ifndef DISTRIC_CLUSTER_CONFIG_MANAGER_H
#define DISTRIC_CLUSTER_CONFIG_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <distric_raft.h>
#include <distric_obs.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* ============================================================================
 * CONSTANTS
 * ========================================================================= */

#define DISTRIC_MAX_NODE_ID_LEN 64
#define DISTRIC_MAX_ADDRESS_LEN 256

/* ============================================================================
 * CONFIGURATION TYPES
 * ========================================================================= */

/**
 * @brief Node role in the cluster
 */
typedef enum {
    CONFIG_NODE_ROLE_COORDINATOR,   /**< Coordinator node (runs Raft) */
    CONFIG_NODE_ROLE_WORKER         /**< Worker node (executes tasks) */
} config_node_role_t;

/**
 * @brief Node configuration entry
 */
typedef struct {
    char node_id[DISTRIC_MAX_NODE_ID_LEN];       /**< Node identifier */
    config_node_role_t role;                      /**< Node role */
    char address[DISTRIC_MAX_ADDRESS_LEN];       /**< Node address */
    uint16_t port;                                /**< Node port */
    bool enabled;                                 /**< Is node enabled? */
} config_node_entry_t;

/**
 * @brief System parameter entry
 */
typedef struct {
    char key[128];                                /**< Parameter key */
    char value[256];                              /**< Parameter value */
    char description[256];                        /**< Parameter description */
} config_param_entry_t;

/**
 * @brief Configuration state (in-memory state machine)
 */
typedef struct {
    /* Version tracking */
    uint32_t version;                             /**< Configuration version */
    uint64_t timestamp_ms;                        /**< Last update timestamp */
    
    /* Node configuration */
    config_node_entry_t* nodes;                   /**< Array of nodes */
    size_t node_count;                            /**< Number of nodes */
    size_t node_capacity;                         /**< Node array capacity */
    
    /* System parameters */
    config_param_entry_t* params;                 /**< Array of parameters */
    size_t param_count;                           /**< Number of parameters */
    size_t param_capacity;                        /**< Parameter array capacity */
    
    /* Default system parameters */
    uint32_t gossip_interval_ms;                  /**< Gossip interval */
    uint32_t gossip_suspect_timeout_ms;           /**< Gossip suspect timeout */
    uint32_t raft_election_timeout_min_ms;        /**< Min election timeout */
    uint32_t raft_election_timeout_max_ms;        /**< Max election timeout */
    uint32_t raft_heartbeat_interval_ms;          /**< Heartbeat interval */
    uint32_t task_assignment_timeout_ms;          /**< Task assignment timeout */
    uint32_t task_execution_timeout_ms;           /**< Task execution timeout */
    uint32_t max_tasks_per_worker;                /**< Max tasks per worker */
} config_state_t;

/**
 * @brief Configuration change operation types
 */
typedef enum {
    CONFIG_OP_ADD_NODE,                           /**< Add a node */
    CONFIG_OP_REMOVE_NODE,                        /**< Remove a node */
    CONFIG_OP_UPDATE_NODE,                        /**< Update node info */
    CONFIG_OP_SET_PARAM,                          /**< Set a parameter */
    CONFIG_OP_DELETE_PARAM,                       /**< Delete a parameter */
    CONFIG_OP_BATCH                               /**< Batch operation */
} config_operation_t;

/**
 * @brief Configuration change request
 */
typedef struct {
    config_operation_t operation;                 /**< Operation type */
    
    /* For node operations */
    config_node_entry_t node;                     /**< Node data */
    
    /* For parameter operations */
    config_param_entry_t param;                   /**< Parameter data */
    
    /* For batch operations */
    void* batch_data;                             /**< Batch data (serialized) */
    size_t batch_size;                            /**< Batch data size */
} config_change_t;

/**
 * @brief Configuration manager instance
 */
typedef struct config_manager config_manager_t;

/* ============================================================================
 * LIFECYCLE FUNCTIONS
 * ========================================================================= */

/**
 * @brief Create a new configuration manager
 * 
 * @param raft Raft node instance (must be already created)
 * @param metrics Metrics registry (optional, can be NULL)
 * @param logger Logger instance (optional, can be NULL)
 * @return Configuration manager instance or NULL on error
 * 
 * Thread Safety: Safe to call from any thread
 */
config_manager_t* config_manager_create(
    raft_node_t* raft,
    metrics_registry_t* metrics,
    logger_t* logger
);

/**
 * @brief Destroy configuration manager
 * 
 * @param manager Configuration manager instance
 * 
 * Thread Safety: Must not be called concurrently with any other operations
 */
void config_manager_destroy(config_manager_t* manager);

/**
 * @brief Start the configuration manager
 * 
 * This function registers the configuration manager as a Raft state machine
 * apply function, so configuration changes are applied when committed by Raft.
 * 
 * @param manager Configuration manager instance
 * @return 0 on success, -1 on error
 * 
 * Thread Safety: Safe to call from any thread
 */
int config_manager_start(config_manager_t* manager);

/**
 * @brief Stop the configuration manager
 * 
 * @param manager Configuration manager instance
 * @return 0 on success, -1 on error
 * 
 * Thread Safety: Safe to call from any thread
 */
int config_manager_stop(config_manager_t* manager);

/* ============================================================================
 * CONFIGURATION PROPOSAL (LEADER-ONLY)
 * ========================================================================= */

/**
 * @brief Propose a configuration change (leader only)
 * 
 * This function submits a configuration change to Raft for replication.
 * Only the leader can propose changes. Followers must redirect to leader.
 * 
 * @param manager Configuration manager instance
 * @param change Configuration change request
 * @param index_out Output for Raft log index (optional, can be NULL)
 * @return 0 on success, -1 on error, -2 if not leader
 * 
 * Thread Safety: Safe to call from any thread
 * 
 * Error Codes:
 * - 0: Success (change submitted to Raft)
 * - -1: Generic error
 * - -2: Not the leader (redirect to leader)
 */
int config_manager_propose_change(
    config_manager_t* manager,
    const config_change_t* change,
    uint32_t* index_out
);

/**
 * @brief Wait for a proposed change to be committed
 * 
 * @param manager Configuration manager instance
 * @param log_index Raft log index from propose_change
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success (committed), -1 on timeout or error
 * 
 * Thread Safety: Safe to call from any thread
 */
int config_manager_wait_committed(
    config_manager_t* manager,
    uint32_t log_index,
    uint32_t timeout_ms
);

/* ============================================================================
 * CONFIGURATION QUERIES (ALL NODES)
 * ========================================================================= */

/**
 * @brief Get current configuration state (read-only copy)
 * 
 * Returns a copy of the current configuration state. The caller is responsible
 * for freeing the returned state using config_state_free().
 * 
 * @param manager Configuration manager instance
 * @param state_out Output for configuration state (caller must free)
 * @return 0 on success, -1 on error
 * 
 * Thread Safety: Safe to call from any thread (concurrent reads allowed)
 */
int config_manager_get_state(
    config_manager_t* manager,
    config_state_t** state_out
);

/**
 * @brief Free a configuration state copy
 * 
 * @param state Configuration state to free
 * 
 * Thread Safety: Safe to call from any thread
 */
void config_state_free(config_state_t* state);

/**
 * @brief Get node entry by ID
 * 
 * @param manager Configuration manager instance
 * @param node_id Node identifier
 * @param node_out Output for node entry (caller must not free)
 * @return 0 on success, -1 if not found
 * 
 * Thread Safety: Safe to call from any thread
 */
int config_manager_get_node(
    config_manager_t* manager,
    const char* node_id,
    config_node_entry_t* node_out
);

/**
 * @brief Get all nodes with a specific role
 * 
 * @param manager Configuration manager instance
 * @param role Node role to filter by
 * @param nodes_out Output array (caller must free with free())
 * @param count_out Output for number of nodes
 * @return 0 on success, -1 on error
 * 
 * Thread Safety: Safe to call from any thread
 */
int config_manager_get_nodes_by_role(
    config_manager_t* manager,
    config_node_role_t role,
    config_node_entry_t** nodes_out,
    size_t* count_out
);

/**
 * @brief Get a system parameter value
 * 
 * @param manager Configuration manager instance
 * @param key Parameter key
 * @param value_out Output buffer for parameter value
 * @param value_size Size of output buffer
 * @return 0 on success, -1 if not found
 * 
 * Thread Safety: Safe to call from any thread
 */
int config_manager_get_param(
    config_manager_t* manager,
    const char* key,
    char* value_out,
    size_t value_size
);

/**
 * @brief Get a system parameter as an integer
 * 
 * @param manager Configuration manager instance
 * @param key Parameter key
 * @param default_value Default value if parameter not found
 * @return Parameter value as integer, or default_value if not found
 * 
 * Thread Safety: Safe to call from any thread
 */
int64_t config_manager_get_param_int(
    config_manager_t* manager,
    const char* key,
    int64_t default_value
);

/**
 * @brief Get configuration version
 * 
 * @param manager Configuration manager instance
 * @return Configuration version number
 * 
 * Thread Safety: Safe to call from any thread
 */
uint32_t config_manager_get_version(config_manager_t* manager);

/**
 * @brief Get the apply function for Raft integration
 * 
 * This function returns the apply callback that should be registered with
 * Raft during Raft node creation. The returned function will be called by
 * Raft when log entries are committed.
 * 
 * @param manager Configuration manager instance
 * @return Apply function pointer
 * 
 * Thread Safety: Safe to call from any thread
 * 
 * Usage:
 * @code
 * void (*apply_fn)(const uint8_t*, size_t, void*);
 * apply_fn = config_manager_get_apply_function(manager);
 * // Pass apply_fn to Raft during configuration
 * @endcode
 */
void (*config_manager_get_apply_function(config_manager_t* manager))(
    const uint8_t* data,
    size_t len,
    void* ctx
);

/* ============================================================================
 * SERIALIZATION (INTERNAL USE)
 * ========================================================================= */

/**
 * @brief Serialize a configuration change for Raft log
 * 
 * @param change Configuration change
 * @param data_out Output buffer (caller must free)
 * @param size_out Output for buffer size
 * @return 0 on success, -1 on error
 */
int config_change_serialize(
    const config_change_t* change,
    uint8_t** data_out,
    size_t* size_out
);

/**
 * @brief Deserialize a configuration change from Raft log
 * 
 * @param data Input buffer
 * @param size Buffer size
 * @param change_out Output for configuration change
 * @return 0 on success, -1 on error
 */
int config_change_deserialize(
    const uint8_t* data,
    size_t size,
    config_change_t* change_out
);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

/**
 * @brief Get string representation of node role
 * 
 * @param role Node role
 * @return String representation
 */
const char* config_node_role_to_string(config_node_role_t role);

/**
 * @brief Parse node role from string
 * 
 * @param str String to parse
 * @param role_out Output for node role
 * @return 0 on success, -1 on error
 */
int config_node_role_from_string(const char* str, config_node_role_t* role_out);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CLUSTER_CONFIG_MANAGER_H */