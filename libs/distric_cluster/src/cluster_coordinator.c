#include "distric_cluster.h"
#include "distric_cluster/worker_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Internal coordinator structure - matches the opaque type from distric_cluster.h */
struct cluster_coordinator_t {
    worker_pool_t* worker_pool;
    
    pthread_mutex_t lock;
    bool running;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    
    /* State and leadership */
    cluster_state_t state;
    bool is_leader;
    
    /* Callbacks */
    cluster_on_became_leader_fn on_became_leader;
    cluster_on_lost_leadership_fn on_lost_leadership;
    cluster_on_worker_joined_fn on_worker_joined;
    cluster_on_worker_failed_fn on_worker_failed;
    void* callback_user_data;
};

/* ============================================================================
 * PUBLIC API IMPLEMENTATION - Matches distric_cluster.h signatures
 * ========================================================================= */

distric_err_t cluster_coordinator_create(
    const cluster_config_t* config,
    cluster_coordinator_t** coord_out
) {
    if (!config || !coord_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    cluster_coordinator_t* coordinator = calloc(1, sizeof(cluster_coordinator_t));
    if (!coordinator) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    coordinator->metrics = config->metrics;
    coordinator->logger = config->logger;
    
    /* Create worker pool */
    coordinator->worker_pool = worker_pool_create(config->metrics, config->logger);
    if (!coordinator->worker_pool) {
        free(coordinator);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    pthread_mutex_init(&coordinator->lock, NULL);
    
    /* Initialize state and callbacks */
    coordinator->state = CLUSTER_STATE_STOPPED;
    coordinator->is_leader = false;
    coordinator->on_became_leader = NULL;
    coordinator->on_lost_leadership = NULL;
    coordinator->on_worker_joined = NULL;
    coordinator->on_worker_failed = NULL;
    coordinator->callback_user_data = NULL;
    coordinator->running = false;
    
    if (coordinator->logger) {
        LOG_INFO(coordinator->logger, "cluster_coordinator", "Cluster coordinator created", NULL);
    }
    
    *coord_out = coordinator;
    return DISTRIC_OK;
}

void cluster_coordinator_destroy(cluster_coordinator_t* coord) {
    if (!coord) {
        return;
    }
    
    if (coord->running) {
        cluster_coordinator_stop(coord);
    }
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster_coordinator", "Destroying cluster coordinator", NULL);
    }
    
    worker_pool_destroy(coord->worker_pool);
    pthread_mutex_destroy(&coord->lock);
    free(coord);
}

distric_err_t cluster_coordinator_start(cluster_coordinator_t* coord) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (coord->running) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    coord->state = CLUSTER_STATE_RUNNING;
    coord->running = true;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster_coordinator", "Cluster coordinator started", NULL);
    }
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_stop(cluster_coordinator_t* coord) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!coord->running) {
        return DISTRIC_ERR_INVALID_STATE;
    }
    
    coord->running = false;
    coord->state = CLUSTER_STATE_STOPPED;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster_coordinator", "Cluster coordinator stopped", NULL);
    }
    
    return DISTRIC_OK;
}

cluster_state_t cluster_coordinator_get_state(const cluster_coordinator_t* coord) {
    if (!coord) {
        return CLUSTER_STATE_UNKNOWN;
    }
    return coord->state;
}

bool cluster_coordinator_is_leader(const cluster_coordinator_t* coord) {
    if (!coord) {
        return false;
    }
    return coord->is_leader;
}

distric_err_t cluster_coordinator_set_leadership_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_became_leader_fn on_became_leader,
    cluster_on_lost_leadership_fn on_lost_leadership,
    void* user_data
) {
    if (!coord) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&coord->lock);
    coord->on_became_leader = on_became_leader;
    coord->on_lost_leadership = on_lost_leadership;
    coord->callback_user_data = user_data;
    pthread_mutex_unlock(&coord->lock);
    
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
    
    pthread_mutex_lock(&coord->lock);
    coord->on_worker_joined = on_worker_joined;
    coord->on_worker_failed = on_worker_failed;
    coord->callback_user_data = user_data;
    pthread_mutex_unlock(&coord->lock);
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_submit_task(
    cluster_coordinator_t* coord,
    const char* node_id,
    cluster_node_type_t node_type,
    const void* task_data,
    size_t task_size,
    uint64_t* task_id_out
) {
    if (!coord || !task_data || !task_id_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Simplified implementation - just generate a task ID */
    static uint64_t next_task_id = 1;
    *task_id_out = __sync_fetch_and_add(&next_task_id, 1);
    
    (void)node_id;
    (void)node_type;
    (void)task_size;
    
    if (coord->logger) {
        LOG_INFO(coord->logger, "cluster_coordinator", "Task submitted", "task_id", NULL);
    }
    
    return DISTRIC_OK;
}

distric_err_t cluster_coordinator_get_workers(
    cluster_coordinator_t* coord,
    cluster_node_t** nodes_out,
    size_t* count_out
) {
    if (!coord || !nodes_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Simplified - return empty list */
    *nodes_out = NULL;
    *count_out = 0;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* cluster_state_to_string(cluster_state_t state) {
    switch (state) {
        case CLUSTER_STATE_UNKNOWN:
            return "UNKNOWN";
        case CLUSTER_STATE_STOPPED:
            return "STOPPED";
        case CLUSTER_STATE_STARTING:
            return "STARTING";
        case CLUSTER_STATE_RUNNING:
            return "RUNNING";
        case CLUSTER_STATE_STOPPING:
            return "STOPPING";
        case CLUSTER_STATE_ERROR:
            return "ERROR";
        case CLUSTER_STATE_DEGRADED:
            return "DEGRADED";
        case CLUSTER_STATE_ACTIVE:
            return "ACTIVE";
        case CLUSTER_STATE_SHUTDOWN:
            return "SHUTDOWN";
        default:
            return "INVALID";
    }
}

const char* cluster_node_type_to_string(cluster_node_type_t type) {
    switch (type) {
        case CLUSTER_NODE_UNKNOWN:
            return "UNKNOWN";
        case CLUSTER_NODE_COORDINATOR:
            return "COORDINATOR";
        case CLUSTER_NODE_WORKER:
            return "WORKER";
        default:
            return "INVALID";
    }
}

const char* distric_cluster_version(void) {
    return "1.0.0";
}