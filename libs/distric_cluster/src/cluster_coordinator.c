#include "distric_cluster/cluster_coordinator.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Internal task tracking structure */
typedef struct task_entry_s {
    task_request_t request;
    task_result_t result;
    task_status_t status;
    char assigned_worker_id[64];
    uint64_t assigned_at_ms;
    struct task_entry_s* next;
} task_entry_t;

/* Internal coordinator structure */
struct cluster_coordinator_s {
    cluster_coordinator_config_t config;
    worker_pool_t* worker_pool;
    
    task_entry_t* pending_tasks;
    task_entry_t* active_tasks;
    task_entry_t* completed_tasks;
    
    pthread_mutex_t lock;
    pthread_t worker_thread;
    bool running;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    
    /* Statistics */
    size_t total_submitted;
    size_t total_completed;
    size_t total_failed;
};

/* Default configuration */
static const cluster_coordinator_config_t DEFAULT_CONFIG = {
    .max_pending_tasks = 1000,
    .max_task_history = 10000,
    .task_timeout_ms = 30000,
    .heartbeat_interval_ms = 1000,
    .worker_timeout_ms = 5000,
    .default_strategy = WORKER_SELECT_LEAST_LOADED,
    .enable_task_retry = true,
    .max_task_retries = 3
};

cluster_coordinator_t* cluster_coordinator_create(
    const cluster_coordinator_config_t* config,
    metrics_registry_t* metrics,
    logger_t* logger
) {
    cluster_coordinator_t* coordinator = calloc(1, sizeof(cluster_coordinator_t));
    if (!coordinator) {
        return NULL;
    }
    
    /* Use provided config or defaults */
    if (config) {
        coordinator->config = *config;
    } else {
        coordinator->config = DEFAULT_CONFIG;
    }
    
    coordinator->metrics = metrics;
    coordinator->logger = logger;
    
    /* Create worker pool */
    coordinator->worker_pool = worker_pool_create(metrics, logger);
    if (!coordinator->worker_pool) {
        free(coordinator);
        return NULL;
    }
    
    pthread_mutex_init(&coordinator->lock, NULL);
    
    if (logger) {
        LOG_INFO(logger, "cluster_coordinator", "Cluster coordinator created", NULL);
    }
    
    return coordinator;
}

void cluster_coordinator_destroy(cluster_coordinator_t* coordinator) {
    if (!coordinator) {
        return;
    }
    
    if (coordinator->running) {
        cluster_coordinator_stop(coordinator);
    }
    
    if (coordinator->logger) {
        LOG_INFO(coordinator->logger, "cluster_coordinator", "Destroying cluster coordinator", NULL);
    }
    
    /* Free task lists */
    task_entry_t* current = coordinator->pending_tasks;
    while (current) {
        task_entry_t* next = current->next;
        free(current->request.task_data);
        free(current->result.result_data);
        free(current);
        current = next;
    }
    
    current = coordinator->active_tasks;
    while (current) {
        task_entry_t* next = current->next;
        free(current->request.task_data);
        free(current->result.result_data);
        free(current);
        current = next;
    }
    
    current = coordinator->completed_tasks;
    while (current) {
        task_entry_t* next = current->next;
        free(current->request.task_data);
        free(current->result.result_data);
        free(current);
        current = next;
    }
    
    worker_pool_destroy(coordinator->worker_pool);
    pthread_mutex_destroy(&coordinator->lock);
    free(coordinator);
}

int cluster_coordinator_start(cluster_coordinator_t* coordinator) {
    if (!coordinator || coordinator->running) {
        return -1;
    }
    
    coordinator->running = true;
    
    if (coordinator->logger) {
        LOG_INFO(coordinator->logger, "cluster_coordinator", "Cluster coordinator started", NULL);
    }
    
    return 0;
}

int cluster_coordinator_stop(cluster_coordinator_t* coordinator) {
    if (!coordinator || !coordinator->running) {
        return -1;
    }
    
    coordinator->running = false;
    
    if (coordinator->logger) {
        LOG_INFO(coordinator->logger, "cluster_coordinator", "Cluster coordinator stopped", NULL);
    }
    
    return 0;
}

int cluster_coordinator_submit_task(
    cluster_coordinator_t* coordinator,
    const task_request_t* request
) {
    if (!coordinator || !request) {
        return -1;
    }
    
    pthread_mutex_lock(&coordinator->lock);
    
    /* Create task entry */
    task_entry_t* entry = calloc(1, sizeof(task_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&coordinator->lock);
        return -1;
    }
    
    entry->request = *request;
    entry->status = TASK_STATUS_PENDING;
    
    /* Copy task data if present */
    if (request->task_data && request->data_size > 0) {
        entry->request.task_data = malloc(request->data_size);
        if (entry->request.task_data) {
            memcpy(entry->request.task_data, request->task_data, request->data_size);
        }
    }
    
    /* Add to pending list */
    entry->next = coordinator->pending_tasks;
    coordinator->pending_tasks = entry;
    coordinator->total_submitted++;
    
    pthread_mutex_unlock(&coordinator->lock);
    
    if (coordinator->logger) {
        LOG_INFO(coordinator->logger, "cluster_coordinator", "Task submitted",
                "task_id", request->task_id, NULL);
    }
    
    return 0;
}

int cluster_coordinator_cancel_task(
    cluster_coordinator_t* coordinator,
    const char* task_id
) {
    if (!coordinator || !task_id) {
        return -1;
    }
    
    pthread_mutex_lock(&coordinator->lock);
    
    /* Search in pending and active tasks */
    task_entry_t* current = coordinator->pending_tasks;
    while (current) {
        if (strcmp(current->request.task_id, task_id) == 0) {
            current->status = TASK_STATUS_CANCELLED;
            pthread_mutex_unlock(&coordinator->lock);
            return 0;
        }
        current = current->next;
    }
    
    current = coordinator->active_tasks;
    while (current) {
        if (strcmp(current->request.task_id, task_id) == 0) {
            current->status = TASK_STATUS_CANCELLED;
            pthread_mutex_unlock(&coordinator->lock);
            return 0;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&coordinator->lock);
    return -1;
}

int cluster_coordinator_get_task_status(
    cluster_coordinator_t* coordinator,
    const char* task_id,
    task_status_t* status_out
) {
    if (!coordinator || !task_id || !status_out) {
        return -1;
    }
    
    pthread_mutex_lock(&coordinator->lock);
    
    /* Search all task lists */
    task_entry_t* current = coordinator->pending_tasks;
    while (current) {
        if (strcmp(current->request.task_id, task_id) == 0) {
            *status_out = current->status;
            pthread_mutex_unlock(&coordinator->lock);
            return 0;
        }
        current = current->next;
    }
    
    current = coordinator->active_tasks;
    while (current) {
        if (strcmp(current->request.task_id, task_id) == 0) {
            *status_out = current->status;
            pthread_mutex_unlock(&coordinator->lock);
            return 0;
        }
        current = current->next;
    }
    
    current = coordinator->completed_tasks;
    while (current) {
        if (strcmp(current->request.task_id, task_id) == 0) {
            *status_out = current->status;
            pthread_mutex_unlock(&coordinator->lock);
            return 0;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&coordinator->lock);
    return -1;
}

int cluster_coordinator_get_task_result(
    cluster_coordinator_t* coordinator,
    const char* task_id,
    task_result_t* result_out
) {
    if (!coordinator || !task_id || !result_out) {
        return -1;
    }
    
    pthread_mutex_lock(&coordinator->lock);
    
    task_entry_t* current = coordinator->completed_tasks;
    while (current) {
        if (strcmp(current->request.task_id, task_id) == 0) {
            *result_out = current->result;
            
            /* Copy result data if present */
            if (current->result.result_data && current->result.data_size > 0) {
                result_out->result_data = malloc(current->result.data_size);
                if (result_out->result_data) {
                    memcpy(result_out->result_data, current->result.result_data, 
                           current->result.data_size);
                }
            }
            
            pthread_mutex_unlock(&coordinator->lock);
            return 0;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&coordinator->lock);
    return -1;
}

int cluster_coordinator_update_from_gossip(
    cluster_coordinator_t* coordinator,
    const gossip_node_info_t* node
) {
    if (!coordinator || !node) {
        return -1;
    }
    
    /* Delegate to worker pool */
    return worker_pool_update_from_gossip(coordinator->worker_pool, node);
}

int cluster_coordinator_get_statistics(
    cluster_coordinator_t* coordinator,
    cluster_statistics_t* stats_out
) {
    if (!coordinator || !stats_out) {
        return -1;
    }
    
    pthread_mutex_lock(&coordinator->lock);
    
    /* Count tasks in each state */
    size_t pending = 0, running = 0;
    task_entry_t* current = coordinator->pending_tasks;
    while (current) {
        if (current->status == TASK_STATUS_PENDING) {
            pending++;
        }
        current = current->next;
    }
    
    current = coordinator->active_tasks;
    while (current) {
        if (current->status == TASK_STATUS_RUNNING) {
            running++;
        }
        current = current->next;
    }
    
    stats_out->pending_tasks = pending;
    stats_out->running_tasks = running;
    stats_out->completed_tasks = coordinator->total_completed;
    stats_out->failed_tasks = coordinator->total_failed;
    
    /* Get worker pool statistics */
    worker_pool_capacity_stats_t pool_stats;
    if (worker_pool_get_capacity_stats(coordinator->worker_pool, &pool_stats) == 0) {
        stats_out->total_workers = pool_stats.total_workers;
        stats_out->active_workers = pool_stats.alive_workers;
    } else {
        stats_out->total_workers = 0;
        stats_out->active_workers = 0;
    }
    
    stats_out->average_task_duration_ms = 0.0;
    stats_out->task_throughput = 0.0;
    
    pthread_mutex_unlock(&coordinator->lock);
    return 0;
}

worker_pool_t* cluster_coordinator_get_worker_pool(
    cluster_coordinator_t* coordinator
) {
    return coordinator ? coordinator->worker_pool : NULL;
}

int cluster_coordinator_set_worker_strategy(
    cluster_coordinator_t* coordinator,
    worker_selection_strategy_t strategy
) {
    if (!coordinator) {
        return -1;
    }
    
    return worker_pool_set_strategy(coordinator->worker_pool, strategy);
}
/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* distric_cluster_version(void) {
    return "1.0.0";
}