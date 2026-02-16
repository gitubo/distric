/**
 * @file config_manager.c
 * @brief DistriC Configuration Manager Implementation
 */

#include <distric_cluster/config_manager.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/**
 * @brief Configuration manager internal structure
 */
struct config_manager {
    /* Raft integration */
    raft_node_t* raft;
    
    /* In-memory state machine */
    config_state_t* current_state;
    pthread_rwlock_t state_lock;
    
    /* Observability */
    metrics_registry_t* metrics;
    logger_t* logger;
    
    /* Metrics */
    metric_t* config_version_metric;
    metric_t* config_changes_total;
    metric_t* config_nodes_total;
    metric_t* config_params_total;
    
    /* State */
    bool running;
};

/**
 * @brief Serialization magic number and version
 */
#define CONFIG_SERIALIZE_MAGIC 0x44435347  /* "DCSG" - DistriC ConfiG */
#define CONFIG_SERIALIZE_VERSION 1

/**
 * @brief Serialization header
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t operation;
    uint32_t data_size;
} __attribute__((packed)) config_serialize_header_t;

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

static void apply_config_change(
    const uint8_t* log_entry,
    size_t len,
    void* ctx
);

static int apply_change_to_state(
    config_state_t* state,
    const config_change_t* change,
    logger_t* logger
);

static uint64_t get_timestamp_ms(void);

/* ============================================================================
 * LIFECYCLE FUNCTIONS
 * ========================================================================= */

config_manager_t* config_manager_create(
    raft_node_t* raft,
    metrics_registry_t* metrics,
    logger_t* logger
) {
    if (!raft) {
        return NULL;
    }
    
    config_manager_t* manager = calloc(1, sizeof(config_manager_t));
    if (!manager) {
        return NULL;
    }
    
    manager->raft = raft;
    manager->metrics = metrics;
    manager->logger = logger;
    
    /* Initialize state lock */
    if (pthread_rwlock_init(&manager->state_lock, NULL) != 0) {
        free(manager);
        return NULL;
    }
    
    /* Create initial state */
    manager->current_state = calloc(1, sizeof(config_state_t));
    if (!manager->current_state) {
        pthread_rwlock_destroy(&manager->state_lock);
        free(manager);
        return NULL;
    }
    
    /* Initialize state with defaults */
    manager->current_state->version = 0;
    manager->current_state->timestamp_ms = get_timestamp_ms();
    
    /* Set default system parameters */
    manager->current_state->gossip_interval_ms = 1000;
    manager->current_state->gossip_suspect_timeout_ms = 5000;
    manager->current_state->raft_election_timeout_min_ms = 150;
    manager->current_state->raft_election_timeout_max_ms = 300;
    manager->current_state->raft_heartbeat_interval_ms = 50;
    manager->current_state->task_assignment_timeout_ms = 5000;
    manager->current_state->task_execution_timeout_ms = 300000;  /* 5 minutes */
    manager->current_state->max_tasks_per_worker = 10;
    
    /* Allocate initial arrays */
    manager->current_state->node_capacity = 16;
    manager->current_state->nodes = calloc(
        manager->current_state->node_capacity,
        sizeof(config_node_entry_t)
    );
    
    manager->current_state->param_capacity = 32;
    manager->current_state->params = calloc(
        manager->current_state->param_capacity,
        sizeof(config_param_entry_t)
    );
    
    if (!manager->current_state->nodes || !manager->current_state->params) {
        free(manager->current_state->nodes);
        free(manager->current_state->params);
        free(manager->current_state);
        pthread_rwlock_destroy(&manager->state_lock);
        free(manager);
        return NULL;
    }
    
    /* Register metrics */
    if (metrics) {
        metric_t* metric = NULL;
        if (metrics_register_gauge(metrics, "config_version",
                                   "Current configuration version",
                                   NULL, 0, &metric) == DISTRIC_OK) {
            manager->config_version_metric = metric;
        }
        if (metrics_register_counter(metrics, "config_changes_total",
                                     "Total configuration changes",
                                     NULL, 0, &metric) == DISTRIC_OK) {
            manager->config_changes_total = metric;
        }
        if (metrics_register_gauge(metrics, "config_nodes_total",
                                   "Total configured nodes",
                                   NULL, 0, &metric) == DISTRIC_OK) {
            manager->config_nodes_total = metric;
        }
        if (metrics_register_gauge(metrics, "config_params_total",
                                   "Total configuration parameters",
                                   NULL, 0, &metric) == DISTRIC_OK) {
            manager->config_params_total = metric;
        }
    }
    
    if (logger) {
        LOG_INFO(logger, "config_manager", "Configuration manager created", NULL);
    }
    
    return manager;
}

void config_manager_destroy(config_manager_t* manager) {
    if (!manager) {
        return;
    }
    
    if (manager->logger) {
        LOG_INFO(manager->logger, "config_manager",
                "Destroying configuration manager", NULL);
    }
    
    /* Free state */
    if (manager->current_state) {
        free(manager->current_state->nodes);
        free(manager->current_state->params);
        free(manager->current_state);
    }
    
    pthread_rwlock_destroy(&manager->state_lock);
    free(manager);
}

int config_manager_start(config_manager_t* manager) {
    if (!manager) {
        return -1;
    }
    
    /* Note: The Raft apply function should be set during Raft node creation
     * by using config_manager_get_apply_function() to get the callback.
     * We don't register it here because the Raft API doesn't provide
     * a post-creation registration mechanism. */
    
    manager->running = true;
    
    if (manager->logger) {
        LOG_INFO(manager->logger, "config_manager",
                "Configuration manager started", NULL);
    }
    
    return 0;
}

int config_manager_stop(config_manager_t* manager) {
    if (!manager) {
        return -1;
    }
    
    manager->running = false;
    
    if (manager->logger) {
        LOG_INFO(manager->logger, "config_manager",
                "Configuration manager stopped", NULL);
    }
    
    return 0;
}

/* ============================================================================
 * CONFIGURATION PROPOSAL
 * ========================================================================= */

int config_manager_propose_change(
    config_manager_t* manager,
    const config_change_t* change,
    uint32_t* index_out
) {
    if (!manager || !change) {
        return -1;
    }
    
    /* Check if we're the leader */
    if (!raft_is_leader(manager->raft)) {
        if (manager->logger) {
            LOG_WARN(manager->logger, "config_manager",
                    "Not leader, cannot propose change", NULL);
        }
        return -2;  /* Not leader */
    }
    
    /* Serialize the change */
    uint8_t* serialized = NULL;
    size_t serialized_size = 0;
    
    int result = config_change_serialize(change, &serialized, &serialized_size);
    if (result != 0) {
        if (manager->logger) {
            LOG_ERROR(manager->logger, "config_manager",
                     "Failed to serialize configuration change", NULL);
        }
        return -1;
    }
    
    /* Append to Raft log */
    uint32_t log_index = 0;
    distric_err_t raft_result = raft_append_entry(manager->raft, serialized, 
                                                   serialized_size, &log_index);
    
    free(serialized);
    
    if (raft_result != DISTRIC_OK) {
        if (manager->logger) {
            LOG_ERROR(manager->logger, "config_manager",
                     "Failed to append to Raft log", NULL);
        }
        return -1;
    }
    
    if (index_out) {
        *index_out = log_index;
    }
    
    if (manager->logger) {
        char index_str[32];
        snprintf(index_str, sizeof(index_str), "%u", log_index);
        LOG_INFO(manager->logger, "config_manager",
                "Configuration change proposed",
                "log_index", index_str,
                NULL);
    }
    
    return 0;
}

int config_manager_wait_committed(
    config_manager_t* manager,
    uint32_t log_index,
    uint32_t timeout_ms
) {
    if (!manager) {
        return -1;
    }
    
    distric_err_t result = raft_wait_committed(manager->raft, log_index, timeout_ms);
    return (result == DISTRIC_OK) ? 0 : -1;
}

/* ============================================================================
 * CONFIGURATION QUERIES
 * ========================================================================= */

int config_manager_get_state(
    config_manager_t* manager,
    config_state_t** state_out
) {
    if (!manager || !state_out) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&manager->state_lock);
    
    /* Allocate copy */
    config_state_t* copy = calloc(1, sizeof(config_state_t));
    if (!copy) {
        pthread_rwlock_unlock(&manager->state_lock);
        return -1;
    }
    
    /* Copy basic fields */
    memcpy(copy, manager->current_state, sizeof(config_state_t));
    
    /* Copy nodes array */
    copy->nodes = NULL;
    copy->params = NULL;
    
    if (manager->current_state->node_count > 0) {
        copy->nodes = malloc(copy->node_count * sizeof(config_node_entry_t));
        if (!copy->nodes) {
            pthread_rwlock_unlock(&manager->state_lock);
            free(copy);
            return -1;
        }
        memcpy(copy->nodes, manager->current_state->nodes,
               copy->node_count * sizeof(config_node_entry_t));
    }
    
    /* Copy params array */
    if (manager->current_state->param_count > 0) {
        copy->params = malloc(copy->param_count * sizeof(config_param_entry_t));
        if (!copy->params) {
            pthread_rwlock_unlock(&manager->state_lock);
            free(copy->nodes);
            free(copy);
            return -1;
        }
        memcpy(copy->params, manager->current_state->params,
               copy->param_count * sizeof(config_param_entry_t));
    }
    
    pthread_rwlock_unlock(&manager->state_lock);
    
    *state_out = copy;
    return 0;
}

void config_state_free(config_state_t* state) {
    if (!state) {
        return;
    }
    
    free(state->nodes);
    free(state->params);
    free(state);
}

int config_manager_get_node(
    config_manager_t* manager,
    const char* node_id,
    config_node_entry_t* node_out
) {
    if (!manager || !node_id || !node_out) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&manager->state_lock);
    
    for (size_t i = 0; i < manager->current_state->node_count; i++) {
        if (strcmp(manager->current_state->nodes[i].node_id, node_id) == 0) {
            memcpy(node_out, &manager->current_state->nodes[i],
                   sizeof(config_node_entry_t));
            pthread_rwlock_unlock(&manager->state_lock);
            return 0;
        }
    }
    
    pthread_rwlock_unlock(&manager->state_lock);
    return -1;
}

int config_manager_get_nodes_by_role(
    config_manager_t* manager,
    config_node_role_t role,
    config_node_entry_t** nodes_out,
    size_t* count_out
) {
    if (!manager || !nodes_out || !count_out) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&manager->state_lock);
    
    /* Count matching nodes */
    size_t count = 0;
    for (size_t i = 0; i < manager->current_state->node_count; i++) {
        if (manager->current_state->nodes[i].role == role) {
            count++;
        }
    }
    
    /* Allocate output array */
    config_node_entry_t* nodes = NULL;
    if (count > 0) {
        nodes = malloc(count * sizeof(config_node_entry_t));
        if (!nodes) {
            pthread_rwlock_unlock(&manager->state_lock);
            return -1;
        }
        
        /* Copy matching nodes */
        size_t idx = 0;
        for (size_t i = 0; i < manager->current_state->node_count; i++) {
            if (manager->current_state->nodes[i].role == role) {
                memcpy(&nodes[idx++], &manager->current_state->nodes[i],
                       sizeof(config_node_entry_t));
            }
        }
    }
    
    pthread_rwlock_unlock(&manager->state_lock);
    
    *nodes_out = nodes;
    *count_out = count;
    return 0;
}

int config_manager_get_param(
    config_manager_t* manager,
    const char* key,
    char* value_out,
    size_t value_size
) {
    if (!manager || !key || !value_out || value_size == 0) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&manager->state_lock);
    
    for (size_t i = 0; i < manager->current_state->param_count; i++) {
        if (strcmp(manager->current_state->params[i].key, key) == 0) {
            strncpy(value_out, manager->current_state->params[i].value,
                   value_size - 1);
            value_out[value_size - 1] = '\0';
            pthread_rwlock_unlock(&manager->state_lock);
            return 0;
        }
    }
    
    pthread_rwlock_unlock(&manager->state_lock);
    return -1;
}

int64_t config_manager_get_param_int(
    config_manager_t* manager,
    const char* key,
    int64_t default_value
) {
    char value_str[64];
    
    if (config_manager_get_param(manager, key, value_str, sizeof(value_str)) == 0) {
        char* endptr = NULL;
        errno = 0;
        long long value = strtoll(value_str, &endptr, 10);
        if (errno == 0 && endptr != value_str) {
            return (int64_t)value;
        }
    }
    
    return default_value;
}

uint32_t config_manager_get_version(config_manager_t* manager) {
    if (!manager) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&manager->state_lock);
    uint32_t version = manager->current_state->version;
    pthread_rwlock_unlock(&manager->state_lock);
    
    return version;
}

/* ============================================================================
 * STATE MACHINE APPLY FUNCTION
 * ========================================================================= */

/**
 * @brief Apply a configuration change from Raft log
 * 
 * This function is called by Raft when a log entry is committed.
 * It deserializes the change and applies it to the configuration state.
 */
static void apply_config_change(
    const uint8_t* log_entry,
    size_t len,
    void* ctx
) {
    config_manager_t* manager = (config_manager_t*)ctx;
    if (!manager || !log_entry) {
        return;
    }
    
    /* Deserialize the change */
    config_change_t change;
    if (config_change_deserialize(log_entry, len, &change) != 0) {
        if (manager->logger) {
            LOG_ERROR(manager->logger, "config_manager",
                     "Failed to deserialize configuration change", NULL);
        }
        return;
    }
    
    /* Apply to state machine */
    pthread_rwlock_wrlock(&manager->state_lock);
    
    int result = apply_change_to_state(manager->current_state, &change,
                                      manager->logger);
    
    if (result == 0) {
        /* Update version and timestamp */
        manager->current_state->version++;
        manager->current_state->timestamp_ms = get_timestamp_ms();
        
        /* Update metrics */
        if (manager->config_version_metric) {
            metrics_gauge_set(manager->config_version_metric,
                            (double)manager->current_state->version);
        }
        if (manager->config_changes_total) {
            metrics_counter_add(manager->config_changes_total, 1);
        }
        if (manager->config_nodes_total) {
            metrics_gauge_set(manager->config_nodes_total,
                            (double)manager->current_state->node_count);
        }
        if (manager->config_params_total) {
            metrics_gauge_set(manager->config_params_total,
                            (double)manager->current_state->param_count);
        }
        
        if (manager->logger) {
            char version_str[32];
            snprintf(version_str, sizeof(version_str), "%u",
                    manager->current_state->version);
            LOG_INFO(manager->logger, "config_manager",
                    "Configuration change applied",
                    "version", version_str,
                    NULL);
        }
    } else {
        if (manager->logger) {
            LOG_ERROR(manager->logger, "config_manager",
                     "Failed to apply configuration change", NULL);
        }
    }
    
    pthread_rwlock_unlock(&manager->state_lock);
}

/**
 * @brief Apply a configuration change to the state machine
 * 
 * Caller must hold write lock on state_lock.
 */
static int apply_change_to_state(
    config_state_t* state,
    const config_change_t* change,
    logger_t* logger
) {
    if (!state || !change) {
        return -1;
    }
    
    switch (change->operation) {
        case CONFIG_OP_ADD_NODE: {
            /* Check if node already exists */
            for (size_t i = 0; i < state->node_count; i++) {
                if (strcmp(state->nodes[i].node_id, change->node.node_id) == 0) {
                    if (logger) {
                        LOG_WARN(logger, "config_manager",
                                "Node already exists, updating instead",
                                "node_id", change->node.node_id,
                                NULL);
                    }
                    /* Update existing node */
                    memcpy(&state->nodes[i], &change->node,
                           sizeof(config_node_entry_t));
                    return 0;
                }
            }
            
            /* Expand array if needed */
            if (state->node_count >= state->node_capacity) {
                size_t new_capacity = state->node_capacity * 2;
                config_node_entry_t* new_nodes = realloc(
                    state->nodes,
                    new_capacity * sizeof(config_node_entry_t)
                );
                if (!new_nodes) {
                    return -1;
                }
                state->nodes = new_nodes;
                state->node_capacity = new_capacity;
            }
            
            /* Add new node */
            memcpy(&state->nodes[state->node_count], &change->node,
                   sizeof(config_node_entry_t));
            state->node_count++;
            
            if (logger) {
                LOG_INFO(logger, "config_manager", "Node added",
                        "node_id", change->node.node_id,
                        NULL);
            }
            break;
        }
        
        case CONFIG_OP_REMOVE_NODE: {
            /* Find and remove node */
            for (size_t i = 0; i < state->node_count; i++) {
                if (strcmp(state->nodes[i].node_id, change->node.node_id) == 0) {
                    /* Shift remaining nodes */
                    memmove(&state->nodes[i], &state->nodes[i + 1],
                           (state->node_count - i - 1) * sizeof(config_node_entry_t));
                    state->node_count--;
                    
                    if (logger) {
                        LOG_INFO(logger, "config_manager", "Node removed",
                                "node_id", change->node.node_id,
                                NULL);
                    }
                    return 0;
                }
            }
            
            if (logger) {
                LOG_WARN(logger, "config_manager", "Node not found for removal",
                        "node_id", change->node.node_id,
                        NULL);
            }
            break;
        }
        
        case CONFIG_OP_UPDATE_NODE: {
            /* Find and update node */
            for (size_t i = 0; i < state->node_count; i++) {
                if (strcmp(state->nodes[i].node_id, change->node.node_id) == 0) {
                    memcpy(&state->nodes[i], &change->node,
                           sizeof(config_node_entry_t));
                    
                    if (logger) {
                        LOG_INFO(logger, "config_manager", "Node updated",
                                "node_id", change->node.node_id,
                                NULL);
                    }
                    return 0;
                }
            }
            
            if (logger) {
                LOG_WARN(logger, "config_manager", "Node not found for update",
                        "node_id", change->node.node_id,
                        NULL);
            }
            break;
        }
        
        case CONFIG_OP_SET_PARAM: {
            /* Check if parameter already exists */
            for (size_t i = 0; i < state->param_count; i++) {
                if (strcmp(state->params[i].key, change->param.key) == 0) {
                    /* Update existing parameter */
                    strncpy(state->params[i].value, change->param.value,
                           sizeof(state->params[i].value) - 1);
                    strncpy(state->params[i].description, change->param.description,
                           sizeof(state->params[i].description) - 1);
                    
                    if (logger) {
                        LOG_INFO(logger, "config_manager", "Parameter updated",
                                "key", change->param.key,
                                "value", change->param.value,
                                NULL);
                    }
                    return 0;
                }
            }
            
            /* Expand array if needed */
            if (state->param_count >= state->param_capacity) {
                size_t new_capacity = state->param_capacity * 2;
                config_param_entry_t* new_params = realloc(
                    state->params,
                    new_capacity * sizeof(config_param_entry_t)
                );
                if (!new_params) {
                    return -1;
                }
                state->params = new_params;
                state->param_capacity = new_capacity;
            }
            
            /* Add new parameter */
            memcpy(&state->params[state->param_count], &change->param,
                   sizeof(config_param_entry_t));
            state->param_count++;
            
            if (logger) {
                LOG_INFO(logger, "config_manager", "Parameter added",
                        "key", change->param.key,
                        "value", change->param.value,
                        NULL);
            }
            break;
        }
        
        case CONFIG_OP_DELETE_PARAM: {
            /* Find and remove parameter */
            for (size_t i = 0; i < state->param_count; i++) {
                if (strcmp(state->params[i].key, change->param.key) == 0) {
                    /* Shift remaining parameters */
                    memmove(&state->params[i], &state->params[i + 1],
                           (state->param_count - i - 1) * sizeof(config_param_entry_t));
                    state->param_count--;
                    
                    if (logger) {
                        LOG_INFO(logger, "config_manager", "Parameter deleted",
                                "key", change->param.key,
                                NULL);
                    }
                    return 0;
                }
            }
            
            if (logger) {
                LOG_WARN(logger, "config_manager", "Parameter not found for deletion",
                        "key", change->param.key,
                        NULL);
            }
            break;
        }
        
        case CONFIG_OP_BATCH:
            /* Batch operations not yet implemented */
            if (logger) {
                LOG_ERROR(logger, "config_manager",
                         "Batch operations not yet implemented", NULL);
            }
            return -1;
    }
    
    return 0;
}

/* ============================================================================
 * SERIALIZATION
 * ========================================================================= */

int config_change_serialize(
    const config_change_t* change,
    uint8_t** data_out,
    size_t* size_out
) {
    if (!change || !data_out || !size_out) {
        return -1;
    }
    
    /* Calculate size needed */
    size_t data_size = 0;
    
    switch (change->operation) {
        case CONFIG_OP_ADD_NODE:
        case CONFIG_OP_REMOVE_NODE:
        case CONFIG_OP_UPDATE_NODE:
            data_size = sizeof(config_node_entry_t);
            break;
        case CONFIG_OP_SET_PARAM:
        case CONFIG_OP_DELETE_PARAM:
            data_size = sizeof(config_param_entry_t);
            break;
        case CONFIG_OP_BATCH:
            data_size = change->batch_size;
            break;
    }
    
    size_t total_size = sizeof(config_serialize_header_t) + data_size;
    
    /* Allocate buffer */
    uint8_t* buffer = malloc(total_size);
    if (!buffer) {
        return -1;
    }
    
    /* Write header */
    config_serialize_header_t* header = (config_serialize_header_t*)buffer;
    header->magic = CONFIG_SERIALIZE_MAGIC;
    header->version = CONFIG_SERIALIZE_VERSION;
    header->operation = (uint32_t)change->operation;
    header->data_size = (uint32_t)data_size;
    
    /* Write data */
    uint8_t* data_ptr = buffer + sizeof(config_serialize_header_t);
    
    switch (change->operation) {
        case CONFIG_OP_ADD_NODE:
        case CONFIG_OP_REMOVE_NODE:
        case CONFIG_OP_UPDATE_NODE:
            memcpy(data_ptr, &change->node, sizeof(config_node_entry_t));
            break;
        case CONFIG_OP_SET_PARAM:
        case CONFIG_OP_DELETE_PARAM:
            memcpy(data_ptr, &change->param, sizeof(config_param_entry_t));
            break;
        case CONFIG_OP_BATCH:
            if (change->batch_data) {
                memcpy(data_ptr, change->batch_data, change->batch_size);
            }
            break;
    }
    
    *data_out = buffer;
    *size_out = total_size;
    return 0;
}

int config_change_deserialize(
    const uint8_t* data,
    size_t size,
    config_change_t* change_out
) {
    if (!data || !change_out || size < sizeof(config_serialize_header_t)) {
        return -1;
    }
    
    /* Read header */
    const config_serialize_header_t* header = (const config_serialize_header_t*)data;
    
    /* Validate magic and version */
    if (header->magic != CONFIG_SERIALIZE_MAGIC) {
        return -1;
    }
    if (header->version != CONFIG_SERIALIZE_VERSION) {
        return -1;
    }
    
    /* Validate size */
    if (size != sizeof(config_serialize_header_t) + header->data_size) {
        return -1;
    }
    
    /* Initialize change */
    memset(change_out, 0, sizeof(config_change_t));
    change_out->operation = (config_operation_t)header->operation;
    
    /* Read data */
    const uint8_t* data_ptr = data + sizeof(config_serialize_header_t);
    
    switch (change_out->operation) {
        case CONFIG_OP_ADD_NODE:
        case CONFIG_OP_REMOVE_NODE:
        case CONFIG_OP_UPDATE_NODE:
            if (header->data_size != sizeof(config_node_entry_t)) {
                return -1;
            }
            memcpy(&change_out->node, data_ptr, sizeof(config_node_entry_t));
            break;
        case CONFIG_OP_SET_PARAM:
        case CONFIG_OP_DELETE_PARAM:
            if (header->data_size != sizeof(config_param_entry_t)) {
                return -1;
            }
            memcpy(&change_out->param, data_ptr, sizeof(config_param_entry_t));
            break;
        case CONFIG_OP_BATCH:
            /* For batch operations, just store pointer and size */
            /* Note: This is not a deep copy, caller must handle */
            change_out->batch_data = (void*)data_ptr;
            change_out->batch_size = header->data_size;
            break;
        default:
            return -1;
    }
    
    return 0;
}

/* ============================================================================
 * APPLY FUNCTION ACCESSOR
 * ========================================================================= */

void (*config_manager_get_apply_function(config_manager_t* manager))(
    const uint8_t* data,
    size_t len,
    void* ctx
) {
    (void)manager;  /* Not used, but kept for API consistency */
    return apply_config_change;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* config_node_role_to_string(config_node_role_t role) {
    switch (role) {
        case CONFIG_NODE_ROLE_COORDINATOR: return "coordinator";
        case CONFIG_NODE_ROLE_WORKER: return "worker";
        default: return "unknown";
    }
}

int config_node_role_from_string(const char* str, config_node_role_t* role_out) {
    if (!str || !role_out) {
        return -1;
    }
    
    if (strcmp(str, "coordinator") == 0) {
        *role_out = CONFIG_NODE_ROLE_COORDINATOR;
        return 0;
    } else if (strcmp(str, "worker") == 0) {
        *role_out = CONFIG_NODE_ROLE_WORKER;
        return 0;
    }
    
    return -1;
}

/**
 * @brief Get current timestamp in milliseconds
 */
static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}