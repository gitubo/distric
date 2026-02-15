#include "distric_cluster/worker_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

/* Internal worker pool structure */
struct worker_pool_s {
    worker_info_t* workers;
    size_t worker_count;
    size_t worker_capacity;
    
    pthread_rwlock_t lock;
    
    worker_selection_strategy_t strategy;
    size_t round_robin_index;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    
    /* Metrics */
    metric_t* worker_count_metric;
    metric_t* worker_alive_metric;
    metric_t* total_capacity_metric;
    metric_t* total_load_metric;
};

/* Helper function to get current time in milliseconds */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Find a worker by ID */
static worker_info_t* find_worker(worker_pool_t* pool, const char* worker_id) {
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (strcmp(pool->workers[i].worker_id, worker_id) == 0) {
            return &pool->workers[i];
        }
    }
    return NULL;
}

/* Update pool metrics */
static void update_pool_metrics(worker_pool_t* pool) {
    if (!pool->metrics) {
        return;
    }
    
    size_t alive_count = 0;
    uint32_t total_capacity = 0;
    uint32_t total_load = 0;
    
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

/* Public API implementation */

worker_pool_t* worker_pool_create(
    metrics_registry_t* metrics,
    logger_t* logger
) {
    worker_pool_t* pool = calloc(1, sizeof(worker_pool_t));
    if (!pool) {
        return NULL;
    }
    
    pool->worker_capacity = 16;  /* Initial capacity */
    pool->workers = calloc(pool->worker_capacity, sizeof(worker_info_t));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }
    
    pthread_rwlock_init(&pool->lock, NULL);
    pool->strategy = WORKER_SELECT_LEAST_LOADED;
    pool->metrics = metrics;
    pool->logger = logger;
    
    /* Register metrics */
    if (metrics) {
        metric_t* metric = NULL;
        if (metrics_register_gauge(metrics, "worker_pool_total_workers", 
                                   "Total number of workers", NULL, 0, &metric) == DISTRIC_OK) {
            pool->worker_count_metric = metric;
        }
        if (metrics_register_gauge(metrics, "worker_pool_alive_workers", 
                                   "Number of alive workers", NULL, 0, &metric) == DISTRIC_OK) {
            pool->worker_alive_metric = metric;
        }
        if (metrics_register_gauge(metrics, "worker_pool_total_capacity", 
                                   "Total task capacity", NULL, 0, &metric) == DISTRIC_OK) {
            pool->total_capacity_metric = metric;
        }
        if (metrics_register_gauge(metrics, "worker_pool_total_load", 
                                   "Total current load", NULL, 0, &metric) == DISTRIC_OK) {
            pool->total_load_metric = metric;
        }
    }
    
    if (logger) {
        LOG_INFO(logger, "worker_pool", "Worker pool created", NULL);
    }
    
    return pool;
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

int worker_pool_update_from_gossip(
    worker_pool_t* pool,
    const gossip_node_info_t* node
) {
    if (!pool || !node) {
        return -1;
    }
    
    /* Only process worker nodes */
    if (node->role != GOSSIP_ROLE_WORKER) {
        return 0;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, node->node_id);
    
    /* Add new worker if not found */
    if (!worker) {
        /* Expand array if needed */
        if (pool->worker_count >= pool->worker_capacity) {
            /* Double capacity */
            size_t new_capacity = pool->worker_capacity * 2;
            worker_info_t* new_workers = realloc(pool->workers,
                                                 new_capacity * sizeof(worker_info_t));
            if (!new_workers) {
                pthread_rwlock_unlock(&pool->lock);
                return -1;
            }
            pool->workers = new_workers;
            pool->worker_capacity = new_capacity;
        }
        
        worker = &pool->workers[pool->worker_count++];
        
        /* Initialize new worker */
        strncpy(worker->worker_id, node->node_id, sizeof(worker->worker_id) - 1);
        strncpy(worker->address, node->address, sizeof(worker->address) - 1);
        worker->port = node->port;
        worker->joined_at_ms = get_time_ms();
        
        /* Default values */
        worker->max_concurrent_tasks = 10;
        
        if (pool->logger) {
            LOG_INFO(pool->logger, "worker_pool", "New worker joined",
                    "worker_id", node->node_id, NULL);
        }
    }
    
    /* Update worker state */
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
    return 0;
}

int worker_pool_mark_failed(
    worker_pool_t* pool,
    const char* worker_id
) {
    if (!pool || !worker_id) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, worker_id);
    if (!worker) {
        pthread_rwlock_unlock(&pool->lock);
        return -1;
    }
    
    worker->status = GOSSIP_NODE_FAILED;
    
    if (pool->logger) {
        char task_count[32];
        snprintf(task_count, sizeof(task_count), "%u", worker->current_task_count);
        LOG_WARN(pool->logger, "worker_pool", "Worker failed",
                "worker_id", worker_id,
                "active_tasks", task_count,
                NULL);
    }
    
    update_pool_metrics(pool);
    
    pthread_rwlock_unlock(&pool->lock);
    return 0;
}

/* Worker selection strategies */

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
    double min_utilization = 1.0;
    
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
    size_t target = rand() % available_count;
    size_t current = 0;
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == GOSSIP_NODE_ALIVE &&
            pool->workers[i].current_task_count < pool->workers[i].max_concurrent_tasks) {
            if (current == target) {
                return &pool->workers[i];
            }
            current++;
        }
    }
    
    return NULL;
}

int worker_pool_select_worker(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy,
    const worker_info_t** worker_out
) {
    if (!pool || !worker_out) {
        return -1;
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
            pthread_rwlock_unlock(&pool->lock);
            return -1;
    }
    
    if (!selected) {
        pthread_rwlock_unlock(&pool->lock);
        return -1;
    }
    
    *worker_out = selected;
    pthread_rwlock_unlock(&pool->lock);
    return 0;
}

int worker_pool_update_task_count(
    worker_pool_t* pool,
    const char* worker_id,
    int32_t task_delta
) {
    if (!pool || !worker_id) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, worker_id);
    if (!worker) {
        pthread_rwlock_unlock(&pool->lock);
        return -1;
    }
    
    /* Update task count */
    if (task_delta > 0) {
        worker->current_task_count += task_delta;
        worker->last_task_assigned_ms = get_time_ms();
    } else {
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
    return 0;
}

int worker_pool_get_workers_by_status(
    worker_pool_t* pool,
    gossip_node_status_t status,
    worker_info_t** workers_out,
    size_t* count_out
) {
    if (!pool || !workers_out || !count_out) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->lock);
    
    /* Count matching workers */
    size_t count = 0;
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == status) {
            count++;
        }
    }
    
    /* Allocate output array */
    worker_info_t* result = NULL;
    if (count > 0) {
        result = malloc(count * sizeof(worker_info_t));
        if (!result) {
            pthread_rwlock_unlock(&pool->lock);
            return -1;
        }
        
        /* Copy matching workers */
        size_t idx = 0;
        for (size_t i = 0; i < pool->worker_count; i++) {
            if (pool->workers[i].status == status) {
                memcpy(&result[idx++], &pool->workers[i], sizeof(worker_info_t));
            }
        }
    }
    
    *workers_out = result;
    *count_out = count;
    
    pthread_rwlock_unlock(&pool->lock);
    return 0;
}

int worker_pool_get_worker_info(
    worker_pool_t* pool,
    const char* worker_id,
    worker_info_t* worker_out
) {
    if (!pool || !worker_id || !worker_out) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->lock);
    
    worker_info_t* worker = find_worker(pool, worker_id);
    if (!worker) {
        pthread_rwlock_unlock(&pool->lock);
        return -1;
    }
    
    memcpy(worker_out, worker, sizeof(worker_info_t));
    
    pthread_rwlock_unlock(&pool->lock);
    return 0;
}

int worker_pool_get_capacity_stats(
    worker_pool_t* pool,
    worker_pool_capacity_stats_t* stats_out
) {
    if (!pool || !stats_out) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->lock);
    
    size_t alive_count = 0;
    uint32_t total_capacity = 0;
    uint32_t total_utilized = 0;
    
    for (size_t i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].status == GOSSIP_NODE_ALIVE) {
            alive_count++;
            total_capacity += pool->workers[i].max_concurrent_tasks;
            total_utilized += pool->workers[i].current_task_count;
        }
    }
    
    stats_out->total_workers = pool->worker_count;
    stats_out->alive_workers = alive_count;
    stats_out->total_capacity = total_capacity;
    stats_out->total_utilized = total_utilized;
    stats_out->utilization_rate = total_capacity > 0 ? 
        (double)total_utilized / (double)total_capacity : 0.0;
    stats_out->average_load = alive_count > 0 ? 
        (double)total_utilized / (double)alive_count : 0.0;
    
    pthread_rwlock_unlock(&pool->lock);
    return 0;
}

int worker_pool_set_strategy(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy
) {
    if (!pool) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&pool->lock);
    pool->strategy = strategy;
    pthread_rwlock_unlock(&pool->lock);
    
    return 0;
}

int worker_pool_get_strategy(
    worker_pool_t* pool,
    worker_selection_strategy_t* strategy
) {
    if (!pool || !strategy) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&pool->lock);
    *strategy = pool->strategy;
    pthread_rwlock_unlock(&pool->lock);
    
    return 0;
}
/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* worker_selection_strategy_to_string(
    worker_selection_strategy_t strategy
) {
    switch (strategy) {
        case WORKER_SELECT_ROUND_ROBIN:
            return "ROUND_ROBIN";
        case WORKER_SELECT_LEAST_LOADED:
            return "LEAST_LOADED";
        case WORKER_SELECT_LEAST_UTILIZED:
            return "LEAST_UTILIZED";
        case WORKER_SELECT_RANDOM:
            return "RANDOM";
        default:
            return "UNKNOWN";
    }
}