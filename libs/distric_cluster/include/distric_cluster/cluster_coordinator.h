#ifndef DISTRIC_CLUSTER_CLUSTER_COORDINATOR_H
#define DISTRIC_CLUSTER_CLUSTER_COORDINATOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "distric_cluster/worker_pool.h"
#include "distric_obs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct cluster_coordinator_s cluster_coordinator_t;
typedef struct task_request_s task_request_t;
typedef struct task_result_s task_result_t;

/* Task status enumeration */
typedef enum {
    TASK_STATUS_PENDING = 0,
    TASK_STATUS_ASSIGNED = 1,
    TASK_STATUS_RUNNING = 2,
    TASK_STATUS_COMPLETED = 3,
    TASK_STATUS_FAILED = 4,
    TASK_STATUS_CANCELLED = 5
} task_status_t;

/* Task priority enumeration */
typedef enum {
    TASK_PRIORITY_LOW = 0,
    TASK_PRIORITY_NORMAL = 1,
    TASK_PRIORITY_HIGH = 2,
    TASK_PRIORITY_CRITICAL = 3
} task_priority_t;

/* Task request structure */
struct task_request_s {
    char task_id[64];
    char task_type[64];
    void* task_data;
    size_t data_size;
    
    task_priority_t priority;
    uint64_t submitted_at_ms;
    uint64_t deadline_ms;
    
    uint32_t max_retries;
    uint32_t retry_count;
};

/* Task result structure */
struct task_result_s {
    char task_id[64];
    char worker_id[64];
    
    task_status_t status;
    void* result_data;
    size_t data_size;
    
    uint64_t started_at_ms;
    uint64_t completed_at_ms;
    
    char error_message[256];
};

/* Cluster coordinator configuration */
typedef struct {
    size_t max_pending_tasks;
    size_t max_task_history;
    
    uint32_t task_timeout_ms;
    uint32_t heartbeat_interval_ms;
    uint32_t worker_timeout_ms;
    
    worker_selection_strategy_t default_strategy;
    
    bool enable_task_retry;
    uint32_t max_task_retries;
} cluster_coordinator_config_t;

/* Cluster statistics */
typedef struct {
    size_t pending_tasks;
    size_t running_tasks;
    size_t completed_tasks;
    size_t failed_tasks;
    
    size_t total_workers;
    size_t active_workers;
    
    double average_task_duration_ms;
    double task_throughput;
} cluster_statistics_t;

/**
 * Create a new cluster coordinator
 * 
 * @param config Configuration parameters
 * @param metrics Optional metrics registry
 * @param logger Optional logger
 * @return New coordinator instance or NULL on error
 */
cluster_coordinator_t* cluster_coordinator_create(
    const cluster_coordinator_config_t* config,
    metrics_registry_t* metrics,
    logger_t* logger
);

/**
 * Destroy a cluster coordinator
 * 
 * @param coordinator Coordinator to destroy
 */
void cluster_coordinator_destroy(cluster_coordinator_t* coordinator);

/**
 * Start the cluster coordinator
 * 
 * @param coordinator Coordinator instance
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_start(cluster_coordinator_t* coordinator);

/**
 * Stop the cluster coordinator
 * 
 * @param coordinator Coordinator instance
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_stop(cluster_coordinator_t* coordinator);

/**
 * Submit a task for execution
 * 
 * @param coordinator Coordinator instance
 * @param request Task request
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_submit_task(
    cluster_coordinator_t* coordinator,
    const task_request_t* request
);

/**
 * Cancel a pending or running task
 * 
 * @param coordinator Coordinator instance
 * @param task_id Task identifier
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_cancel_task(
    cluster_coordinator_t* coordinator,
    const char* task_id
);

/**
 * Get the status of a task
 * 
 * @param coordinator Coordinator instance
 * @param task_id Task identifier
 * @param status_out Output for task status
 * @return 0 on success, -1 if task not found
 */
int cluster_coordinator_get_task_status(
    cluster_coordinator_t* coordinator,
    const char* task_id,
    task_status_t* status_out
);

/**
 * Get the result of a completed task
 * 
 * @param coordinator Coordinator instance
 * @param task_id Task identifier
 * @param result_out Output for task result (caller must free result_data)
 * @return 0 on success, -1 if task not found or not completed
 */
int cluster_coordinator_get_task_result(
    cluster_coordinator_t* coordinator,
    const char* task_id,
    task_result_t* result_out
);

/**
 * Update coordinator with gossip protocol information
 * 
 * @param coordinator Coordinator instance
 * @param node Gossip node information
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_update_from_gossip(
    cluster_coordinator_t* coordinator,
    const gossip_node_info_t* node
);

/**
 * Get cluster statistics
 * 
 * @param coordinator Coordinator instance
 * @param stats_out Output for statistics
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_get_statistics(
    cluster_coordinator_t* coordinator,
    cluster_statistics_t* stats_out
);

/**
 * Get the worker pool managed by this coordinator
 * 
 * @param coordinator Coordinator instance
 * @return Worker pool pointer or NULL
 */
worker_pool_t* cluster_coordinator_get_worker_pool(
    cluster_coordinator_t* coordinator
);

/**
 * Set the worker selection strategy
 * 
 * @param coordinator Coordinator instance
 * @param strategy Strategy to use
 * @return 0 on success, -1 on error
 */
int cluster_coordinator_set_worker_strategy(
    cluster_coordinator_t* coordinator,
    worker_selection_strategy_t strategy
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CLUSTER_CLUSTER_COORDINATOR_H */