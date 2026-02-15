#ifndef CLUSTER_COORDINATOR_INTERNAL_H
#define CLUSTER_COORDINATOR_INTERNAL_H

#include "distric_cluster.h"
#include "distric_raft.h"
#include "distric_gossip.h"
#include "distric_obs.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for worker pool */
typedef struct worker_pool_t worker_pool_t;

/**
 * Internal structure for cluster coordinator
 * This is private and should not be exposed in the public API
 */
struct cluster_coordinator_t {
    /* Configuration */
    cluster_config_t config;
    
    /* State */
    cluster_state_t state;
    bool is_leader;
    pthread_mutex_t leadership_lock;
    
    /* Components */
    gossip_t* gossip;
    raft_t* raft;
    worker_pool_t* worker_pool;
    
    /* Metrics */
    metrics_registry_t* metrics;
    metric_t* is_leader_metric;
    metric_t* coordinator_count_metric;
    metric_t* worker_count_metric;
    
    /* Logging */
    logger_t* logger;
    
    /* Leadership callbacks */
    cluster_on_became_leader_fn on_became_leader;
    cluster_on_lost_leadership_fn on_lost_leadership;
    void* leadership_callback_data;
    
    /* Worker callbacks */
    cluster_on_worker_joined_fn on_worker_joined;
    cluster_on_worker_failed_fn on_worker_failed;
    void* worker_callback_data;
};

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_COORDINATOR_INTERNAL_H */