/**
 * @file cluster_coordinator.c
 * @brief Cluster Coordinator Implementation - Raft + Gossip Integration
 * 
 * Orchestrates both Raft and Gossip protocols to provide unified cluster management.
 * 
 * @version 1.0
 * @date 2026-02-12
 */

#include "distric_cluster.h"
#include "worker_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

struct cluster_coordinator_s {
    cluster_config_t config;
    cluster_state_t state;
    
    /* Protocols */
    raft_node_t* raft;              /* NULL for workers */
    gossip_state_t* gossip;
    worker_pool_t* worker_pool;     /* NULL for workers */
    
    /* Leadership state */
    bool is_leader;
    pthread_mutex_t leadership_lock;
    
    /* Callbacks */
    cluster_on_became_leader_fn on_became_leader;
    cluster_on_lost_leadership_fn on_lost_leadership;
    void* leadership_callback_data;
    
    cluster_on_worker_joined_fn on_worker_joined;
    cluster_on_worker_failed_fn on_worker_failed;
    void* worker_callback_data;
    
    /* Metrics */
    metrics_registry_t* metrics;
    metric_t* is_leader_metric;
    metric_t* coordinator_count_metric;
    metric_t* worker_count_metric;
    
    /* Logging */
    logger_t* logger;
};

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

static void on_raft_became_leader(raft_node_t* raft, void* userdata);
static void on_raft_became_follower(raft_node_t* raft, void* userdata);
static void on_gossip_node_joined(const gossip_node_info_t* node, void* userdata);
static void on_gossip_node_suspected(const gossip_node_info_t* node, void* userdata);
static void on_gossip_node_failed(const gossip_node_info_t* node, void* userdata);
static void on_gossip_node_recovered(const gossip_node_info_t* node, void* userdata);

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

distric_err_t cluster_coordinator_create(
    const cluster_config_t* config,
    cluster_coordinator_t** coord_out
) {
    if (!config || !coord_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    cluster_coordinator_t* coord = calloc(1, sizeof(cluster_coordinator_t));
    if (!coord) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(&coord->config, config, sizeof(cluster_config_t));
    coord->state = CLUSTER_STATE_STOPPED;
    pthread_mutex_init(&coord->leadership_lock, NULL);
    
    coord->metrics = config->metrics;
    coord->logger = config->logger;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Creating cluster coordinator",
                "node_id", config->node_id,
                "node_type", cluster_node_type_to_string(config->node_type),
                NULL);
    }
    
    /* Create Gossip (all nodes) */
    gossip_config_t gossip_cfg = {
        .node_id = "",
        .bind_address = "",
        .bind_port = config->gossip_port,
        .role = (config->node_type == CLUSTER_NODE_COORDINATOR) ?
                GOSSIP_ROLE_COORDINATOR : GOSSIP_ROLE_WORKER,
        .protocol_period_ms = config->gossip_interval_ms > 0 ?
                             config->gossip_interval_ms : 1000,
        .probe_timeout_ms = 500,
        .indirect_probe_count = 3,
        .suspicion_multiplier = 3,
        .max_transmissions = 3,
        .metrics = config->metrics,
        .logger = config->logger
    };
    
    strncpy(gossip_cfg.node_id, config->node_id, sizeof(gossip_cfg.node_id) - 1);
    strncpy(gossip_cfg.bind_address, config->bind_address, sizeof(gossip_cfg.bind_address) - 1);
    
    distric_err_t err = gossip_create(&gossip_cfg, &coord->gossip);
    if (err != DISTRIC_OK) {
        if (coord->logger) {
            LOG_ERROR(coord->logger, "cluster", "Failed to create gossip", NULL);
        }
        free(coord);
        return err;
    }
    
    /* Set gossip callbacks */
    gossip_set_on_node_joined(coord->gossip, on_gossip_node_joined, coord);
    gossip_set_on_node_suspected(coord->gossip, on_gossip_node_suspected, coord);
    gossip_set_on_node_failed(coord->gossip, on_gossip_node_failed, coord);
    gossip_set_on_node_recovered(coord->gossip, on_gossip_node_recovered, coord);
    
    /* Add gossip seeds */
    for (size_t i = 0; i < config->gossip_seed_count; i++) {
        char address[256];
        uint16_t port;
        
        /* Parse "host:port" */
        if (sscanf(config->gossip_seeds[i], "%255[^:]:%hu", address, &port) == 2) {
            gossip_add_seed(coord->gossip, address, port);
        }
    }
    
    /* Create Raft (coordinators only) */
    if (config->node_type == CLUSTER_NODE_COORDINATOR) {
        raft_config_t raft_cfg = {
            .node_id = "",
            .election_timeout_min_ms = config->raft_election_timeout_min_ms > 0 ?
                                      config->raft_election_timeout_min_ms : 150,
            .election_timeout_max_ms = config->raft_election_timeout_max_ms > 0 ?
                                      config->raft_election_timeout_max_ms : 300,
            .heartbeat_interval_ms = config->raft_heartbeat_interval_ms > 0 ?
                                    config->raft_heartbeat_interval_ms : 50,
            .peers = NULL,
            .peer_count = 0,
            .apply_fn = NULL,  /* TODO: Implement state machine */
            .apply_userdata = NULL,
            .metrics = config->metrics,
            .logger = config->logger,
            .storage_path = ""
        };
        
        strncpy(raft_cfg.node_id, config->node_id, sizeof(raft_cfg.node_id) - 1);
        strncpy(raft_cfg.storage_path, config->storage_path, sizeof(raft_cfg.storage_path) - 1);
        
        /* Parse Raft peers */
        raft_peer_t* peers = NULL;
        if (config->raft_peer_count > 0) {
            peers = calloc(config->raft_peer_count, sizeof(raft_peer_t));
            if (!peers) {
                gossip_destroy(coord->gossip);
                free(coord);
                return DISTRIC_ERR_NO_MEMORY;
            }
            
            for (size_t i = 0; i < config->raft_peer_count; i++) {
                /* Parse "node_id@host:port" or "host:port" */
                char* at_pos = strchr(config->raft_peers[i], '@');
                if (at_pos) {
                    size_t id_len = at_pos - config->raft_peers[i];
                    if (id_len < sizeof(peers[i].node_id)) {
                        memcpy(peers[i].node_id, config->raft_peers[i], id_len);
                        peers[i].node_id[id_len] = '\0';
                    }
                    sscanf(at_pos + 1, "%255[^:]:%hu", peers[i].address, &peers[i].port);
                } else {
                    snprintf(peers[i].node_id, sizeof(peers[i].node_id), "peer-%zu", i);
                    sscanf(config->raft_peers[i], "%255[^:]:%hu", peers[i].address, &peers[i].port);
                }
            }
        }
        
        raft_cfg.peers = peers;
        raft_cfg.peer_count = config->raft_peer_count;
        
        err = raft_create(&raft_cfg, &coord->raft);
        free(peers);
        
        if (err != DISTRIC_OK) {
            if (coord->logger) {
                LOG_ERROR(coord->logger, "cluster", "Failed to create Raft", NULL);
            }
            gossip_destroy(coord->gossip);
            free(coord);
            return err;
        }
        
        /* Set Raft callbacks */
        raft_set_leadership_callbacks(coord->raft,
                                     on_raft_became_leader,
                                     on_raft_became_follower,
                                     coord);
        
        /* Create worker pool (leaders use this) */
        err = worker_pool_create(config->metrics, config->logger, &coord->worker_pool);
        if (err != DISTRIC_OK) {
            if (coord->logger) {
                LOG_ERROR(coord->logger, "cluster", "Failed to create worker pool", NULL);
            }
            raft_destroy(coord->raft);
            gossip_destroy(coord->gossip);
            free(coord);
            return err;
        }
    }
    
    /* Register metrics */
    if (coord->metrics) {
        metric_t* metric;
        
        metrics_register_gauge(coord->metrics, "cluster_is_leader",
                              "1 if this node is Raft leader", NULL, 0, &metric);
        coord->is_leader_metric = metric;
        
        metrics_register_gauge(coord->metrics, "cluster_coordinators_total",
                              "Total coordinator nodes", NULL, 0, &metric);
        coord->coordinator_count_metric = metric;
        
        metrics_register_gauge(coord->metrics, "cluster_workers_total",
                              "Total worker nodes", NULL, 0, &metric);
        coord->worker_count_metric = metric;
    }
    
    *coord_out = coord;
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_start(cluster_coordinator_t* coord) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (coord->state != CLUSTER_STATE_STOPPED) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Starting cluster coordinator", NULL);
    }
    
    coord->state = CLUSTER_STATE_STARTING;
    
    /* Start Raft (coordinators only) */
    if (coord->raft) {
        distric_err_t err = raft_start(coord->raft);
        if (err != DISTRIC_OK) {
            if (coord->logger) {
                LOG_ERROR(coord->logger, "cluster", "Failed to start Raft", NULL);
            }
            coord->state = CLUSTER_STATE_ERROR;
            return err;
        }
    }
    
    /* Start Gossip */
    distric_err_t err = gossip_start(coord->gossip);
    if (err != DISTRIC_OK) {
        if (coord->logger) {
            LOG_ERROR(coord->logger, "cluster", "Failed to start Gossip", NULL);
        }
        if (coord->raft) {
            raft_stop(coord->raft);
        }
        coord->state = CLUSTER_STATE_ERROR;
        return err;
    }
    
    coord->state = CLUSTER_STATE_RUNNING;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Cluster coordinator started", NULL);
    }
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_stop(cluster_coordinator_t* coord) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (coord->state != CLUSTER_STATE_RUNNING) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Stopping cluster coordinator", NULL);
    }
    
    coord->state = CLUSTER_STATE_STOPPING;
    
    /* Stop Gossip (graceful leave) */
    gossip_leave(coord->gossip);
    gossip_stop(coord->gossip);
    
    /* Stop Raft (step down if leader) */
    if (coord->raft) {
        raft_stop(coord->raft);
    }
    
    coord->state = CLUSTER_STATE_STOPPED;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Cluster coordinator stopped", NULL);
    }
    
    return DISTRIC_OK;
}

void cluster_coordinator_destroy(cluster_coordinator_t* coord) {
    if (!coord) {
        return;
    }
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Destroying cluster coordinator", NULL);
    }
    
    if (coord->worker_pool) {
        worker_pool_destroy(coord->worker_pool);
    }
    
    if (coord->raft) {
        raft_destroy(coord->raft);
    }
    
    if (coord->gossip) {
        gossip_destroy(coord->gossip);
    }
    
    pthread_mutex_destroy(&coord->leadership_lock);
    free(coord);
}

/* ============================================================================
 * RAFT CALLBACKS
 * ========================================================================= */

static void on_raft_became_leader(raft_node_t* raft, void* userdata) {
    (void)raft;
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    
    pthread_mutex_lock(&coord->leadership_lock);
    coord->is_leader = true;
    pthread_mutex_unlock(&coord->leadership_lock);
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Became Raft leader", NULL);
    }
    
    if (coord->is_leader_metric) {
        metrics_gauge_set(coord->is_leader_metric, 1);
    }
    
    /* Invoke user callback */
    if (coord->on_became_leader) {
        coord->on_became_leader(coord, coord->leadership_callback_data);
    }
}

static void on_raft_became_follower(raft_node_t* raft, void* userdata) {
    (void)raft;
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    
    pthread_mutex_lock(&coord->leadership_lock);
    bool was_leader = coord->is_leader;
    coord->is_leader = false;
    pthread_mutex_unlock(&coord->leadership_lock);
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Lost Raft leadership", NULL);
    }
    
    if (coord->is_leader_metric) {
        metrics_gauge_set(coord->is_leader_metric, 0);
    }
    
    /* Invoke user callback */
    if (was_leader && coord->on_lost_leadership) {
        coord->on_lost_leadership(coord, coord->leadership_callback_data);
    }
}

/* ============================================================================
 * GOSSIP CALLBACKS
 * ========================================================================= */

static void on_gossip_node_joined(const gossip_node_info_t* node, void* userdata) {
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Node joined via gossip",
                "node_id", node->node_id,
                "role", gossip_role_to_string(node->role),
                NULL);
    }
    
    /* Update worker pool if this is a worker */
    if (node->role == GOSSIP_ROLE_WORKER && coord->worker_pool) {
        worker_pool_update_from_gossip(coord->worker_pool, node);
        
        /* Invoke user callback */
        if (coord->on_worker_joined) {
            cluster_node_t worker_node;
            memset(&worker_node, 0, sizeof(worker_node));
            strncpy(worker_node.node_id, node->node_id, sizeof(worker_node.node_id) - 1);
            strncpy(worker_node.address, node->address, sizeof(worker_node.address) - 1);
            worker_node.gossip_port = node->port;
            worker_node.node_type = CLUSTER_NODE_WORKER;
            worker_node.gossip_status = node->status;
            
            coord->on_worker_joined(coord, &worker_node, coord->worker_callback_data);
        }
    }
}

static void on_gossip_node_suspected(const gossip_node_info_t* node, void* userdata) {
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    
    if (coord->logger) {
        LOG_WARN(coord->logger, "cluster", "Node suspected via gossip",
                "node_id", node->node_id,
                NULL);
    }
    
    /* Update worker pool */
    if (node->role == GOSSIP_ROLE_WORKER && coord->worker_pool) {
        worker_pool_update_from_gossip(coord->worker_pool, node);
    }
}

static void on_gossip_node_failed(const gossip_node_info_t* node, void* userdata) {
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    
    if (coord->logger) {
        LOG_ERROR(coord->logger, "cluster", "Node failed via gossip",
                 "node_id", node->node_id,
                 "role", gossip_role_to_string(node->role),
                 NULL);
    }
    
    /* Update worker pool if this is a worker */
    if (node->role == GOSSIP_ROLE_WORKER && coord->worker_pool) {
        worker_pool_mark_failed(coord->worker_pool, node->node_id);
        
        /* Invoke user callback */
        if (coord->on_worker_failed) {
            cluster_node_t worker_node;
            memset(&worker_node, 0, sizeof(worker_node));
            strncpy(worker_node.node_id, node->node_id, sizeof(worker_node.node_id) - 1);
            strncpy(worker_node.address, node->address, sizeof(worker_node.address) - 1);
            worker_node.gossip_port = node->port;
            worker_node.node_type = CLUSTER_NODE_WORKER;
            worker_node.gossip_status = GOSSIP_NODE_FAILED;
            
            coord->on_worker_failed(coord, &worker_node, coord->worker_callback_data);
        }
    }
}

static void on_gossip_node_recovered(const gossip_node_info_t* node, void* userdata) {
    cluster_coordinator_t* coord = (cluster_coordinator_t*)userdata;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster", "Node recovered via gossip",
                "node_id", node->node_id,
                NULL);
    }
    
    /* Update worker pool */
    if (node->role == GOSSIP_ROLE_WORKER && coord->worker_pool) {
        worker_pool_update_from_gossip(coord->worker_pool, node);
    }
}

/* ============================================================================
 * STATE QUERIES
 * ========================================================================= */

distric_err_t cluster_coordinator_get_state(
    const cluster_coordinator_t* coord,
    cluster_state_t* state_out
) {
    if (!coord || !state_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    *state_out = coord->state;
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_is_leader(
    const cluster_coordinator_t* coord,
    bool* is_leader_out
) {
    if (!coord || !is_leader_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!coord->raft) {
        return DISTRIC_ERR_INVALID_STATE;  /* Not a coordinator */
    }
    
    pthread_mutex_lock((pthread_mutex_t*)&coord->leadership_lock);
    *is_leader_out = coord->is_leader;
    pthread_mutex_unlock((pthread_mutex_t*)&coord->leadership_lock);
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_get_leader_id(
    const cluster_coordinator_t* coord,
    char* leader_id_out,
    size_t buffer_size
) {
    if (!coord || !leader_id_out || buffer_size == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!coord->raft) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    /* TODO: Implement raft_get_leader_id() */
    snprintf(leader_id_out, buffer_size, "unknown");
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_get_node_count(
    const cluster_coordinator_t* coord,
    cluster_node_type_t node_type,
    size_t* count_out
) {
    if (!coord || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Query from gossip */
    size_t total_count = 0;
    gossip_get_node_count(coord->gossip, (gossip_node_status_t)-1, &total_count);
    
    /* TODO: Filter by node type using gossip role */
    *count_out = total_count;
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_get_alive_workers(
    const cluster_coordinator_t* coord,
    cluster_node_t** workers_out,
    size_t* count_out
) {
    if (!coord || !workers_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!coord->worker_pool) {
        *workers_out = NULL;
        *count_out = 0;
        return DISTRIC_OK;
    }
    
    worker_info_t* workers = NULL;
    size_t count = 0;
    
    distric_err_t err = worker_pool_get_all_workers(coord->worker_pool, &workers, &count);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Convert to cluster_node_t */
    cluster_node_t* cluster_nodes = calloc(count, sizeof(cluster_node_t));
    if (!cluster_nodes && count > 0) {
        free(workers);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    for (size_t i = 0; i < count; i++) {
        strncpy(cluster_nodes[i].node_id, workers[i].worker_id,
                sizeof(cluster_nodes[i].node_id) - 1);
        strncpy(cluster_nodes[i].address, workers[i].address,
                sizeof(cluster_nodes[i].address) - 1);
        cluster_nodes[i].gossip_port = workers[i].port;
        cluster_nodes[i].node_type = CLUSTER_NODE_WORKER;
        cluster_nodes[i].gossip_status = workers[i].status;
        cluster_nodes[i].last_seen_ms = workers[i].last_seen_ms;
        cluster_nodes[i].load = workers[i].current_task_count;
        cluster_nodes[i].max_concurrent_tasks = workers[i].max_concurrent_tasks;
    }
    
    free(workers);
    *workers_out = cluster_nodes;
    *count_out = count;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * WORKER MANAGEMENT
 * ========================================================================= */

distric_err_t cluster_coordinator_select_worker(
    cluster_coordinator_t* coord,
    char* worker_id_out,
    size_t buffer_size
) {
    if (!coord || !worker_id_out || buffer_size == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!coord->is_leader) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    if (!coord->worker_pool) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    const worker_info_t* worker = NULL;
    distric_err_t err = worker_pool_get_available(coord->worker_pool,
                                                  WORKER_SELECT_LEAST_LOADED,
                                                  &worker);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    strncpy(worker_id_out, worker->worker_id, buffer_size - 1);
    worker_id_out[buffer_size - 1] = '\0';
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_update_worker_load(
    cluster_coordinator_t* coord,
    const char* worker_id,
    int32_t load_delta
) {
    if (!coord || !worker_id) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!coord->worker_pool) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    return worker_pool_update_task_count(coord->worker_pool, worker_id, load_delta);
}

/* ============================================================================
 * CALLBACKS
 * ========================================================================= */

distric_err_t cluster_coordinator_set_leadership_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_became_leader_fn on_became_leader,
    cluster_on_lost_leadership_fn on_lost_leadership,
    void* user_data
) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    coord->on_became_leader = on_became_leader;
    coord->on_lost_leadership = on_lost_leadership;
    coord->leadership_callback_data = user_data;
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_set_worker_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_worker_joined_fn on_worker_joined,
    cluster_on_worker_failed_fn on_worker_failed,
    void* user_data
) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    coord->on_worker_joined = on_worker_joined;
    coord->on_worker_failed = on_worker_failed;
    coord->worker_callback_data = user_data;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* cluster_state_to_string(cluster_state_t state) {
    switch (state) {
        case CLUSTER_STATE_STOPPED:  return "STOPPED";
        case CLUSTER_STATE_STARTING: return "STARTING";
        case CLUSTER_STATE_RUNNING:  return "RUNNING";
        case CLUSTER_STATE_STOPPING: return "STOPPING";
        case CLUSTER_STATE_ERROR:    return "ERROR";
        default:                     return "UNKNOWN";
    }
}

const char* cluster_node_type_to_string(cluster_node_type_t type) {
    switch (type) {
        case CLUSTER_NODE_COORDINATOR: return "COORDINATOR";
        case CLUSTER_NODE_WORKER:      return "WORKER";
        default:                       return "UNKNOWN";
    }
}

const char* distric_cluster_version(void) {
    return "1.0.0";
}