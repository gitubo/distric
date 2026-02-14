/**
 * @file worker_pool.c
 * @brief Worker Pool Management Implementation
 * 
 * Thread-safe worker pool with Gossip integration.
 * 
 * @version 1.0
 * @date 2026-02-12
 */

#include "worker_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

struct worker_pool_s {
    worker_info_t* workers;
    size_t worker_count;
    size_t worker_capacity;
    
    pthread_rwlock_t lock;
    
    worker_selection_strategy_t strategy;
    size_t round_robin_index;
    
    /* Metrics */
    metrics_registry_t* metrics;
    metric_t* worker_count_metric;
    metric_t* worker_alive_metric;
    metric_t* total_capacity_metric;
    metric_t* total_load_metric;
    
    /* Logging */
    logger_t* logger;
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static worker_info_t* find_worker(worker_pool_t* pool, const char* worker_id) {
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (strcmp(pool->workers[i].worker_id, worker_id) == 0) {
            return &pool->workers[i];
        }
    }
    return NULL;
}

static void update_pool_metrics(worker_pool_t* pool) {
    if (!pool->metrics) {
        return;
    }
    
    size_t alive_count = 0;
    uint64_t total_capacity = 0;
    uint64_t total_load = 0;
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == GOSSIP_NODE_ALIVE) {
            alive_count++;
            total_capacity += pool->workers[i].max_concurrent_tasks;
            total_load += pool->workers[i].current_task_count;
        }
    }
    
    if (pool->worker_count_metric) {
        metrics_gauge_set(pool->worker_count_metric, pool->worker_count);
    }
    if (pool->worker_alive_metric) {
        metrics_gauge_set(pool->worker_alive_metric, alive_count);
    }
    if (pool->total_capacity_metric) {
        metrics_gauge_set(pool->total_capacity_metric, total_capacity);
    }
    if (pool->total_load_metric) {
        metrics_gauge_set(pool->total_load_metric, total_load);
    }
}

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

distric_err_t worker_pool_create(
    metrics_registry_t* metrics,
    logger_t* logger,
    worker_pool_t** pool_out
) {
    if (!pool_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    worker_pool_t* pool = calloc(1, sizeof(worker_pool_t));
    if (!pool) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    pool->worker_capacity = 16;  /* Initial capacity */
    pool->workers = calloc(pool->worker_capacity, sizeof(worker_info_t));
    if (!pool->workers) {
        free(pool);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    pthread_rwlock_init(&pool->lock, NULL);
    pool->strategy = WORKER_SELECT_LEAST_LOADED;
    pool->metrics = metrics;
    pool->logger = logger;
    
    /* Register metrics */
    if (metrics) {
        metric_t* metric;
        
        metrics_register_gauge(metrics, "cluster_workers_total",
                              "Total workers in pool", NULL, 0, &metric);
        pool->worker_count_metric = metric;
        
        metrics_register_gauge(metrics, "cluster_workers_alive",
                              "Alive workers", NULL, 0, &metric);
        pool->worker_alive_metric = metric;
        
        metrics_register_gauge(metrics, "cluster_workers_capacity_total",
                              "Total worker capacity", NULL, 0, &metric);
        pool->total_capacity_metric = metric;
        
        metrics_register_gauge(metrics, "cluster_workers_load_total",
                              "Total worker load", NULL, 0, &metric);
        pool->total_load_metric = metric;
    }
    
    if (logger) {
        LOG_INFO(logger, "worker_pool", "Worker pool created", NULL);
    }
    
    *pool_out = pool;
    return DISTRIC_OK;
}

void worker_pool_destroy(worker_pool_t* pool) {
    if (!pool) {
        return;
    }
    
    if (pool->logger) {
        LOG_INFO(pool->logger, "worker_pool", "Destroying worker pool", NULL);
    }
    
    free(pool->workers);
    pthread_rwlock_destroy(&pool->lock);
    free(pool);
}

/* ============================================================================
 * GOSSIP INTEGRATION
 * ========================================================================= */

distric_err_t worker_pool_update_from_gossip(
    worker_pool_t* pool,
    const gossip_node_info_t* node
) {
    if (!pool || !node) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Only track worker nodes */
    if (node->role != GOSSIP_ROLE_WORKER) {
        return DISTRIC_OK;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, node->node_id);
    
    if (!worker) {
        /* New worker - add it */
        if (pool->worker_count >= pool->worker_capacity) {
            /* Expand capacity */
            size_t new_capacity = pool->worker_capacity * 2;
            worker_info_t* new_workers = realloc(pool->workers,
                                                 new_capacity * sizeof(worker_info_t));
            if (!new_workers) {
                pthread_rwlock_unlock(&pool->lock);
                return DISTRIC_ERR_NO_MEMORY;
            }
            pool->workers = new_workers;
            pool->worker_capacity = new_capacity;
        }
        
        worker = &pool->workers[pool->worker_count++];
        memset(worker, 0, sizeof(worker_info_t));
        
        strncpy(worker->worker_id, node->node_id, sizeof(worker->worker_id) - 1);
        strncpy(worker->address, node->address, sizeof(worker->address) - 1);
        worker->port = node->port;
        worker->joined_at_ms = get_time_ms();
        
        /* Default capacity (will be updated via Raft state machine) */
        worker->max_concurrent_tasks = 10;
        
        if (pool->logger) {
            LOG_INFO(pool->logger, "worker_pool", "New worker joined",
                    "worker_id", node->node_id, NULL);
        }
    }
    
    /* Update health status */
    worker->status = node->status;
    worker->last_seen_ms = get_time_ms();
    worker->incarnation = node->incarnation;
    worker->load_metric = node->load;
    
    /* Update utilization */
    if (worker->max_concurrent_tasks > 0) {
        worker->utilization = (double)worker->current_task_count / 
                             (double)worker->max_concurrent_tasks;
    }
    
    update_pool_metrics(pool);
    
    pthread_rwlock_unlock(&pool->lock);
    
    return DISTRIC_OK;
}

distric_err_t worker_pool_mark_failed(
    worker_pool_t* pool,
    const char* worker_id
) {
    if (!pool || !worker_id) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, worker_id);
    if (!worker) {
        pthread_rwlock_unlock(&pool->lock);
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    worker->status = GOSSIP_NODE_FAILED;
    
    if (pool->logger) {
        char task_count[32];
        snprintf(task_count, sizeof(task_count), "%u", worker->current_task_count);
        LOG_WARN(pool->logger, "worker_pool", "Worker failed",
                "worker_id", worker_id,
                "pending_tasks", task_count,
                NULL);
    }
    
    update_pool_metrics(pool);
    
    pthread_rwlock_unlock(&pool->lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * WORKER SELECTION
 * ========================================================================= */

static const worker_info_t* select_round_robin(worker_pool_t* pool) {
    size_t attempts = 0;
    while (attempts < pool->worker_count) {
        size_t idx = pool->round_robin_index % pool->worker_count;
        pool->round_robin_index++;
        
        worker_info_t* worker = &pool->workers[idx];
        if (worker->status == GOSSIP_NODE_ALIVE &&
            worker->current_task_count < worker->max_concurrent_tasks) {
            return worker;
        }
        
        attempts++;
    }
    
    return NULL;
}

static const worker_info_t* select_least_loaded(worker_pool_t* pool) {
    const worker_info_t* best = NULL;
    uint32_t min_load = UINT32_MAX;
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        worker_info_t* worker = &pool->workers[i];
        if (worker->status == GOSSIP_NODE_ALIVE &&
            worker->current_task_count < worker->max_concurrent_tasks &&
            worker->current_task_count < min_load) {
            best = worker;
            min_load = worker->current_task_count;
        }
    }
    
    return best;
}

static const worker_info_t* select_least_utilized(worker_pool_t* pool) {
    const worker_info_t* best = NULL;
    double min_utilization = 2.0;  /* > 1.0 */
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        worker_info_t* worker = &pool->workers[i];
        if (worker->status == GOSSIP_NODE_ALIVE &&
            worker->current_task_count < worker->max_concurrent_tasks &&
            worker->utilization < min_utilization) {
            best = worker;
            min_utilization = worker->utilization;
        }
    }
    
    return best;
}

static const worker_info_t* select_random(worker_pool_t* pool) {
    /* Count available workers */
    size_t available_count = 0;
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == GOSSIP_NODE_ALIVE &&
            pool->workers[i].current_task_count < pool->workers[i].max_concurrent_tasks) {
            available_count++;
        }
    }
    
    if (available_count == 0) {
        return NULL;
    }
    
    /* Select random available worker */
    size_t random_idx = rand() % available_count;
    size_t current_idx = 0;
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == GOSSIP_NODE_ALIVE &&
            pool->workers[i].current_task_count < pool->workers[i].max_concurrent_tasks) {
            if (current_idx == random_idx) {
                return &pool->workers[i];
            }
            current_idx++;
        }
    }
    
    return NULL;
}

distric_err_t worker_pool_get_available(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy,
    const worker_info_t** worker_out
) {
    if (!pool || !worker_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&pool->lock);
    
    const worker_info_t* selected = NULL;
    
    switch (strategy) {
        case WORKER_SELECT_ROUND_ROBIN:
            selected = select_round_robin(pool);
            break;
        case WORKER_SELECT_LEAST_LOADED:
            selected = select_least_loaded(pool);
            break;
        case WORKER_SELECT_LEAST_UTILIZED:
            selected = select_least_utilized(pool);
            break;
        case WORKER_SELECT_RANDOM:
            selected = select_random(pool);
            break;
        default:
            selected = select_least_loaded(pool);
    }
    
    pthread_rwlock_unlock(&pool->lock);
    
    if (!selected) {
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    *worker_out = selected;
    return DISTRIC_OK;
}

/* ============================================================================
 * LOAD MANAGEMENT
 * ========================================================================= */

distric_err_t worker_pool_update_task_count(
    worker_pool_t* pool,
    const char* worker_id,
    int32_t task_delta
) {
    if (!pool || !worker_id) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, worker_id);
    if (!worker) {
        pthread_rwlock_unlock(&pool->lock);
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    /* Update task count */
    if (task_delta > 0) {
        worker->current_task_count += task_delta;
        worker->last_task_assigned_ms = get_time_ms();
    } else if (task_delta < 0) {
        if (worker->current_task_count >= (uint32_t)(-task_delta)) {
            worker->current_task_count += task_delta;
            worker->total_tasks_completed++;
        }
    }
    
    /* Update utilization */
    if (worker->max_concurrent_tasks > 0) {
        worker->utilization = (double)worker->current_task_count / 
                             (double)worker->max_concurrent_tasks;
    }
    
    update_pool_metrics(pool);
    
    pthread_rwlock_unlock(&pool->lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * QUERIES
 * ========================================================================= */

distric_err_t worker_pool_get_count(
    const worker_pool_t* pool,
    gossip_node_status_t status,
    size_t* count_out
) {
    if (!pool || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&pool->lock);
    
    size_t count = 0;
    for (size_t i = 0; i < pool->worker_count; i++) {
        if ((int)status == -1 || pool->workers[i].status == status) {
            count++;
        }
    }
    
    *count_out = count;
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&pool->lock);
    
    return DISTRIC_OK;
}

distric_err_t worker_pool_get_all_workers(
    const worker_pool_t* pool,
    worker_info_t** workers_out,
    size_t* count_out
) {
    if (!pool || !workers_out || !count_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&pool->lock);
    
    worker_info_t* workers = malloc(pool->worker_count * sizeof(worker_info_t));
    if (!workers && pool->worker_count > 0) {
        pthread_rwlock_unlock((pthread_rwlock_t*)&pool->lock);
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    memcpy(workers, pool->workers, pool->worker_count * sizeof(worker_info_t));
    *workers_out = workers;
    *count_out = pool->worker_count;
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&pool->lock);
    
    return DISTRIC_OK;
}

distric_err_t worker_pool_get_worker(
    const worker_pool_t* pool,
    const char* worker_id,
    worker_info_t* worker_out
) {
    if (!pool || !worker_id || !worker_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&pool->lock);
    
    worker_info_t* worker = find_worker((worker_pool_t*)pool, worker_id);
    if (!worker) {
        pthread_rwlock_unlock((pthread_rwlock_t*)&pool->lock);
        return DISTRIC_ERR_NOT_FOUND;
    }
    
    memcpy(worker_out, worker, sizeof(worker_info_t));
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&pool->lock);
    
    return DISTRIC_OK;
}

distric_err_t worker_pool_get_capacity_stats(
    const worker_pool_t* pool,
    uint64_t* total_capacity_out,
    uint64_t* total_utilized_out
) {
    if (!pool) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock((pthread_rwlock_t*)&pool->lock);
    
    uint64_t total_capacity = 0;
    uint64_t total_utilized = 0;
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == GOSSIP_NODE_ALIVE) {
            total_capacity += pool->workers[i].max_concurrent_tasks;
            total_utilized += pool->workers[i].current_task_count;
        }
    }
    
    if (total_capacity_out) {
        *total_capacity_out = total_capacity;
    }
    if (total_utilized_out) {
        *total_utilized_out = total_utilized;
    }
    
    pthread_rwlock_unlock((pthread_rwlock_t*)&pool->lock);
    
    return DISTRIC_OK;
}

distric_err_t worker_pool_set_strategy(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy
) {
    if (!pool) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    pool->strategy = strategy;
    pthread_rwlock_unlock(&pool->lock);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* worker_selection_strategy_to_string(
    worker_selection_strategy_t strategy
) {
    switch (strategy) {
        case WORKER_SELECT_ROUND_ROBIN:    return "ROUND_ROBIN";
        case WORKER_SELECT_LEAST_LOADED:   return "LEAST_LOADED";
        case WORKER_SELECT_LEAST_UTILIZED: return "LEAST_UTILIZED";
        case WORKER_SELECT_RANDOM:         return "RANDOM";
        default:                           return "UNKNOWN";
    }
}