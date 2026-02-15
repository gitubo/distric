#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include "distric_obs.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct worker_pool_t worker_pool_t;

/**
 * Create a worker pool
 * 
 * @param metrics Metrics registry for tracking pool metrics
 * @param logger Logger for pool operations
 * @param pool_out Output parameter for created pool
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t worker_pool_create(
    metrics_registry_t* metrics,
    logger_t* logger,
    worker_pool_t** pool_out
);

/**
 * Destroy a worker pool
 * 
 * @param pool The worker pool to destroy
 */
void worker_pool_destroy(worker_pool_t* pool);

/**
 * Add a worker to the pool
 * 
 * @param pool The worker pool
 * @param node_id ID of the worker node
 * @param address Worker's network address
 * @param port Worker's network port
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t worker_pool_add_worker(
    worker_pool_t* pool,
    const char* node_id,
    const char* address,
    uint16_t port
);

/**
 * Remove a worker from the pool
 * 
 * @param pool The worker pool
 * @param node_id ID of the worker node to remove
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t worker_pool_remove_worker(
    worker_pool_t* pool,
    const char* node_id
);

/**
 * Mark a worker as unhealthy
 * 
 * @param pool The worker pool
 * @param node_id ID of the worker node
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t worker_pool_mark_worker_unhealthy(
    worker_pool_t* pool,
    const char* node_id
);

/**
 * Mark a worker as healthy
 * 
 * @param pool The worker pool
 * @param node_id ID of the worker node
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t worker_pool_mark_worker_healthy(
    worker_pool_t* pool,
    const char* node_id
);

#ifdef __cplusplus
}
#endif

#endif /* WORKER_POOL_H */