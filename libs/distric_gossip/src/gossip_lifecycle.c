/**
 * @file gossip_lifecycle.c
 * @brief Lifecycle management and public API implementations
 * 
 * Implements:
 * - Initialization and destruction
 * - Start/stop protocol
 * - Public query APIs
 * - Callback registration
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#include "gossip_internal.h"
#include "distric_obs.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* ============================================================================
 * INITIALIZATION
 * ========================================================================= */

distric_err_t gossip_init(const gossip_config_t* config, gossip_state_t** state_out) {
    if (!config || !state_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (strlen(config->node_id) == 0 || config->bind_port == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Allocate state */
    gossip_state_t* state = (gossip_state_t*)calloc(1, sizeof(gossip_state_t));
    if (!state) {
        return DISTRIC_ERR_MEMORY;
    }
    
    /* Copy configuration */
    memcpy(&state->config, config, sizeof(gossip_config_t));
    
    /* Set defaults */
    if (state->config.protocol_period_ms == 0) {
        state->config.protocol_period_ms = 1000;  /* 1 second */
    }
    if (state->config.probe_timeout_ms == 0) {
        state->config.probe_timeout_ms = 500;  /* 500ms */
    }
    if (state->config.indirect_probes == 0) {
        state->config.indirect_probes = 3;
    }
    if (state->config.suspicion_mult == 0) {
        state->config.suspicion_mult = 3;
    }
    if (state->config.max_transmissions == 0) {
        state->config.max_transmissions = 3;
    }
    
    /* Initialize UDP socket */
    state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->udp_socket < 0) {
        free(state);
        return DISTRIC_ERR_IO;
    }
    
    /* Set socket to non-blocking */
    int flags = 1;
    setsockopt(state->udp_socket, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags));
    
    /* Bind socket */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config->bind_port);
    
    if (strlen(config->bind_address) == 0 || strcmp(config->bind_address, "0.0.0.0") == 0) {
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, config->bind_address, &bind_addr.sin_addr) <= 0) {
            close(state->udp_socket);
            free(state);
            return DISTRIC_ERR_INVALID_ARG;
        }
    }
    
    if (bind(state->udp_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        close(state->udp_socket);
        free(state);
        return DISTRIC_ERR_IO;
    }
    
    /* Initialize membership */
    pthread_mutex_init(&state->membership_lock, NULL);
    state->membership = NULL;
    state->membership_count = 0;
    state->membership_capacity = 0;
    
    /* Add self to membership */
    gossip_node_info_t self_info;
    memset(&self_info, 0, sizeof(self_info));
    strncpy(self_info.node_id, config->node_id, sizeof(self_info.node_id) - 1);
    strncpy(self_info.address, config->bind_address, sizeof(self_info.address) - 1);
    self_info.port = config->bind_port;
    self_info.status = GOSSIP_NODE_ALIVE;
    self_info.role = config->role;
    self_info.incarnation = 0;
    self_info.load = 0;
    
    membership_entry_t* self_entry = membership_add(state, &self_info);
    if (self_entry) {
        self_entry->local_node = true;
    }
    
    /* Initialize state */
    state->self_incarnation = 0;
    state->self_load = 0;
    atomic_init(&state->sequence_counter, 0);
    atomic_init(&state->running, false);
    
    /* Initialize metrics if available */
    if (config->metrics) {
        metrics_registry_t* metrics = (metrics_registry_t*)config->metrics;
        
        state->ping_sent_metric = metrics_register_counter(
            metrics, "gossip_pings_sent_total",
            "Total PING messages sent", NULL
        );
        
        state->ping_recv_metric = metrics_register_counter(
            metrics, "gossip_pings_received_total",
            "Total PING messages received", NULL
        );
        
        state->suspicions_metric = metrics_register_counter(
            metrics, "gossip_suspicions_total",
            "Total node suspicions", NULL
        );
        
        state->failures_metric = metrics_register_counter(
            metrics, "gossip_failures_total",
            "Total node failures", NULL
        );
    }
    
    /* Log initialization */
    if (config->logger) {
        logger_t* logger = (logger_t*)config->logger;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", config->bind_port);
        
        LOG_INFO(logger, "gossip", "Gossip initialized",
                "node_id", config->node_id,
                "bind_port", port_str, NULL);
    }
    
    *state_out = state;
    return DISTRIC_OK;
}

/* ============================================================================
 * START/STOP
 * ========================================================================= */

distric_err_t gossip_start(gossip_state_t* state) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (atomic_load(&state->running)) {
        return DISTRIC_ERR_INVALID_STATE;  /* Already running */
    }
    
    /* Join cluster via seed nodes */
    if (state->config.seed_count > 0) {
        for (size_t i = 0; i < state->config.seed_count; i++) {
            gossip_node_info_t seed;
            memset(&seed, 0, sizeof(seed));
            
            snprintf(seed.node_id, sizeof(seed.node_id), "seed-%zu", i);
            strncpy(seed.address, state->config.seed_addresses[i], sizeof(seed.address) - 1);
            seed.port = state->config.seed_ports[i];
            seed.status = GOSSIP_NODE_ALIVE;
            seed.role = GOSSIP_ROLE_COORDINATOR;  /* Assume seeds are coordinators */
            
            pthread_mutex_lock(&state->membership_lock);
            membership_entry_t* existing = membership_find(state, seed.node_id);
            if (!existing) {
                membership_add(state, &seed);
            }
            pthread_mutex_unlock(&state->membership_lock);
        }
    }
    
    /* Start protocol threads */
    atomic_store(&state->running, true);
    
    if (pthread_create(&state->protocol_thread, NULL, gossip_protocol_thread, state) != 0) {
        atomic_store(&state->running, false);
        return DISTRIC_ERR_THREAD;
    }
    
    if (pthread_create(&state->receiver_thread, NULL, gossip_receiver_thread, state) != 0) {
        atomic_store(&state->running, false);
        pthread_cancel(state->protocol_thread);
        pthread_join(state->protocol_thread, NULL);
        return DISTRIC_ERR_THREAD;
    }
    
    /* Log start */
    if (state->config.logger) {
        logger_t* logger = (logger_t*)state->config.logger;
        LOG_INFO(logger, "gossip", "Gossip protocol started", NULL);
    }
    
    return DISTRIC_OK;
}

distric_err_t gossip_stop(gossip_state_t* state) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!atomic_load(&state->running)) {
        return DISTRIC_OK;  /* Already stopped */
    }
    
    /* Signal threads to stop */
    atomic_store(&state->running, false);
    
    /* Wait for threads */
    pthread_join(state->protocol_thread, NULL);
    pthread_join(state->receiver_thread, NULL);
    
    /* Log stop */
    if (state->config.logger) {
        logger_t* logger = (logger_t*)state->config.logger;
        LOG_INFO(logger, "gossip", "Gossip protocol stopped", NULL);
    }
    
    return DISTRIC_OK;
}

distric_err_t gossip_leave(gossip_state_t* state) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Broadcast LEAVE message to all alive nodes */
    /* TODO: Implement LEAVE broadcast */
    
    /* Stop protocol */
    return gossip_stop(state);
}

void gossip_destroy(gossip_state_t* state) {
    if (!state) {
        return;
    }
    
    /* Stop if running */
    if (atomic_load(&state->running)) {
        gossip_stop(state);
    }
    
    /* Close socket */
    if (state->udp_socket >= 0) {
        close(state->udp_socket);
    }
    
    /* Free membership */
    pthread_mutex_lock(&state->membership_lock);
    free(state->membership);
    state->membership = NULL;
    state->membership_count = 0;
    state->membership_capacity = 0;
    pthread_mutex_unlock(&state->membership_lock);
    
    pthread_mutex_destroy(&state->membership_lock);
    
    /* Log destruction */
    if (state->config.logger) {
        logger_t* logger = (logger_t*)state->config.logger;
        LOG_INFO(logger, "gossip", "Gossip destroyed", NULL);
    }
    
    free(state);
}

/* ============================================================================
 * MEMBERSHIP QUERIES
 * ========================================================================= */

distric_err_t gossip_get_alive_nodes(
    gossip_state_t* state,
    gossip_node_info_t** nodes_out,
    size_t* count_out
) {
    if (!state || !nodes_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&state->membership_lock);
    
    /* Count alive nodes */
    size_t alive_count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE) {
            alive_count++;
        }
    }
    
    if (alive_count == 0) {
        *nodes_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_OK;
    }
    
    /* Allocate array */
    gossip_node_info_t* nodes = (gossip_node_info_t*)malloc(alive_count * sizeof(gossip_node_info_t));
    if (!nodes) {
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_ERR_MEMORY;
    }
    
    /* Copy alive nodes */
    size_t idx = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE) {
            memcpy(&nodes[idx], &state->membership[i].info, sizeof(gossip_node_info_t));
            idx++;
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
    
    *nodes_out = nodes;
    *count_out = alive_count;
    
    return DISTRIC_OK;
}

distric_err_t gossip_get_nodes_by_role(
    gossip_state_t* state,
    gossip_node_role_t role,
    gossip_node_info_t** nodes_out,
    size_t* count_out
) {
    if (!state || !nodes_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&state->membership_lock);
    
    /* Count matching nodes */
    size_t match_count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE &&
            state->membership[i].info.role == role) {
            match_count++;
        }
    }
    
    if (match_count == 0) {
        *nodes_out = NULL;
        *count_out = 0;
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_OK;
    }
    
    /* Allocate and copy */
    gossip_node_info_t* nodes = (gossip_node_info_t*)malloc(match_count * sizeof(gossip_node_info_t));
    if (!nodes) {
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_ERR_MEMORY;
    }
    
    size_t idx = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if (state->membership[i].info.status == GOSSIP_NODE_ALIVE &&
            state->membership[i].info.role == role) {
            memcpy(&nodes[idx], &state->membership[i].info, sizeof(gossip_node_info_t));
            idx++;
        }
    }
    
    pthread_mutex_unlock(&state->membership_lock);
    
    *nodes_out = nodes;
    *count_out = match_count;
    
    return DISTRIC_OK;
}

distric_err_t gossip_is_node_alive(
    gossip_state_t* state,
    const char* node_id,
    bool* is_alive_out
) {
    if (!state || !node_id || !is_alive_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    *is_alive_out = (entry && entry->info.status == GOSSIP_NODE_ALIVE);
    
    pthread_mutex_unlock(&state->membership_lock);
    
    return DISTRIC_OK;
}

distric_err_t gossip_get_node_info(
    gossip_state_t* state,
    const char* node_id,
    gossip_node_info_t* info_out
) {
    if (!state || !node_id || !info_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&state->membership_lock);
    
    membership_entry_t* entry = membership_find(state, node_id);
    
    if (!entry) {
        pthread_mutex_unlock(&state->membership_lock);
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    memcpy(info_out, &entry->info, sizeof(gossip_node_info_t));
    
    pthread_mutex_unlock(&state->membership_lock);
    
    return DISTRIC_OK;
}

distric_err_t gossip_get_node_count(
    gossip_state_t* state,
    gossip_node_status_t status,
    size_t* count_out
) {
    if (!state || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&state->membership_lock);
    
    size_t count = 0;
    for (size_t i = 0; i < state->membership_count; i++) {
        if ((int)status == -1 || state->membership[i].info.status == status) {
            count++;
        }
    }
    
    *count_out = count;
    
    pthread_mutex_unlock(&state->membership_lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * CALLBACKS
 * ========================================================================= */

distric_err_t gossip_set_on_node_joined(
    gossip_state_t* state,
    gossip_node_joined_fn callback,
    void* user_data
) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    state->on_joined = callback;
    state->on_joined_data = user_data;
    
    return DISTRIC_OK;
}

distric_err_t gossip_set_on_node_suspected(
    gossip_state_t* state,
    gossip_node_suspected_fn callback,
    void* user_data
) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    state->on_suspected = callback;
    state->on_suspected_data = user_data;
    
    return DISTRIC_OK;
}

distric_err_t gossip_set_on_node_failed(
    gossip_state_t* state,
    gossip_node_failed_fn callback,
    void* user_data
) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    state->on_failed = callback;
    state->on_failed_data = user_data;
    
    return DISTRIC_OK;
}

distric_err_t gossip_set_on_node_recovered(
    gossip_state_t* state,
    gossip_node_recovered_fn callback,
    void* user_data
) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    state->on_recovered = callback;
    state->on_recovered_data = user_data;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * STATE MODIFICATION
 * ========================================================================= */

distric_err_t gossip_update_load(gossip_state_t* state, uint64_t load) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    state->self_load = load;
    
    pthread_mutex_lock(&state->membership_lock);
    membership_entry_t* self = membership_find(state, state->config.node_id);
    if (self) {
        self->info.load = load;
        self->transmit_count = state->config.max_transmissions;
    }
    pthread_mutex_unlock(&state->membership_lock);
    
    return DISTRIC_OK;
}

distric_err_t gossip_refute_suspicion(gossip_state_t* state) {
    if (!state) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    state->self_incarnation++;
    
    pthread_mutex_lock(&state->membership_lock);
    membership_entry_t* self = membership_find(state, state->config.node_id);
    if (self) {
        self->info.incarnation = state->self_incarnation;
        self->info.status = GOSSIP_NODE_ALIVE;
        self->transmit_count = state->config.max_transmissions;
    }
    pthread_mutex_unlock(&state->membership_lock);
    
    return DISTRIC_OK;
}

distric_err_t gossip_add_seed(
    gossip_state_t* state,
    const char* address,
    uint16_t port
) {
    if (!state || !address) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    gossip_node_info_t seed;
    memset(&seed, 0, sizeof(seed));
    
    snprintf(seed.node_id, sizeof(seed.node_id), "seed-%s:%u", address, port);
    strncpy(seed.address, address, sizeof(seed.address) - 1);
    seed.port = port;
    seed.status = GOSSIP_NODE_ALIVE;
    seed.role = GOSSIP_ROLE_COORDINATOR;
    
    pthread_mutex_lock(&state->membership_lock);
    membership_entry_t* existing = membership_find(state, seed.node_id);
    if (!existing) {
        membership_add(state, &seed);
    }
    pthread_mutex_unlock(&state->membership_lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* gossip_status_to_string(gossip_node_status_t status) {
    switch (status) {
        case GOSSIP_NODE_ALIVE:     return "ALIVE";
        case GOSSIP_NODE_SUSPECTED: return "SUSPECTED";
        case GOSSIP_NODE_FAILED:    return "FAILED";
        case GOSSIP_NODE_LEFT:      return "LEFT";
        default:                    return "UNKNOWN";
    }
}

const char* gossip_role_to_string(gossip_node_role_t role) {
    switch (role) {
        case GOSSIP_ROLE_COORDINATOR: return "COORDINATOR";
        case GOSSIP_ROLE_WORKER:      return "WORKER";
        default:                      return "UNKNOWN";
    }
}