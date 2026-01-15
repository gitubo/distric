/**
 * @file tcp_pool.h
 * @brief TCP Connection Pool API
 * 
 * Provides efficient connection reuse to reduce TCP handshake overhead.
 * Thread-safe connection pooling with configurable limits.
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_TRANSPORT_TCP_POOL_H
#define DISTRIC_TRANSPORT_TCP_POOL_H

#include "tcp.h"
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

typedef struct tcp_pool_s tcp_pool_t;

/* ============================================================================
 * TCP POOL API
 * ========================================================================= */

/**
 * @brief Create a new TCP connection pool
 * 
 * @param max_connections Maximum number of pooled connections
 * @param metrics Metrics registry for observability
 * @param logger Logger instance
 * @param pool [out] Pointer to store created pool
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t tcp_pool_create(
    size_t max_connections,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_pool_t** pool
);

/**
 * @brief Acquire a connection from the pool
 * 
 * If a connection to the specified (host, port) exists in the pool,
 * it will be reused. Otherwise, a new connection is created.
 * 
 * @param pool The connection pool
 * @param host Hostname or IP address
 * @param port Port number
 * @param conn [out] Pointer to store acquired connection
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t tcp_pool_acquire(
    tcp_pool_t* pool,
    const char* host,
    uint16_t port,
    tcp_connection_t** conn
);

/**
 * @brief Release a connection back to the pool
 * 
 * The connection will be kept alive for reuse if the pool is not full.
 * Otherwise, it will be closed.
 * 
 * @param pool The connection pool
 * @param conn The connection to release
 */
void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn);

/**
 * @brief Get pool statistics
 * 
 * @param pool The connection pool
 * @param size_out [out] Current number of pooled connections
 * @param hits_out [out] Number of connection reuses (cache hits)
 * @param misses_out [out] Number of new connections created (cache misses)
 */
void tcp_pool_get_stats(
    tcp_pool_t* pool,
    size_t* size_out,
    uint64_t* hits_out,
    uint64_t* misses_out
);

/**
 * @brief Destroy the connection pool
 * 
 * All pooled connections will be closed.
 * 
 * @param pool The connection pool
 */
void tcp_pool_destroy(tcp_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_POOL_H */