/**
 * @file tcp_pool.h
 * @brief DistriC TCP Connection Pool API v2
 *
 * Thread-safe connection pooling with LRU eviction and pool-wide metrics.
 * Compatible with the v2 non-blocking tcp.h API.
 *
 * Pool lifecycle:
 *   tcp_pool_create → tcp_pool_acquire → use → tcp_pool_release / tcp_pool_mark_failed
 *   → tcp_pool_destroy
 *
 * Connection acquisition:
 *   If a healthy, idle connection to (host, port) exists it is returned
 *   immediately (cache hit). Otherwise a new connection is established
 *   (cache miss). All connections in the pool use the default send-queue
 *   configuration unless overridden via tcp_pool_config_t.
 *
 * @version 2.0.0
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
 * POOL CONFIGURATION
 * ========================================================================= */

/**
 * @brief Configuration for a TCP connection pool.
 *
 * Zero-initialise to use safe defaults.
 */
typedef struct {
    /**
     * Maximum number of pooled (idle) connections.
     * Connections beyond this limit are closed on release.
     */
    size_t max_connections;

    /**
     * Connection timeout in milliseconds for new connections.
     * 0 = 5000 ms default.
     */
    int connect_timeout_ms;

    /**
     * Per-connection send queue and HWM configuration.
     * Applied to all connections acquired from this pool.
     */
    tcp_connection_config_t conn_config;
} tcp_pool_config_t;

/* ============================================================================
 * TCP POOL API
 * ========================================================================= */

/**
 * @brief Create a TCP connection pool.
 *
 * @param max_connections  Maximum idle connections in the pool.
 * @param metrics          Metrics registry (may be NULL).
 * @param logger           Logger instance (may be NULL).
 * @param pool             [out] Created pool handle.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_pool_create(
    size_t              max_connections,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_pool_t**        pool
);

/**
 * @brief Create a TCP connection pool with explicit configuration.
 *
 * @param config  Pool configuration.
 * @param metrics Metrics registry (may be NULL).
 * @param logger  Logger instance (may be NULL).
 * @param pool    [out] Created pool handle.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_pool_create_with_config(
    const tcp_pool_config_t* config,
    metrics_registry_t*      metrics,
    logger_t*                logger,
    tcp_pool_t**             pool
);

/**
 * @brief Acquire a connection to (host, port).
 *
 * Returns an idle pooled connection if available (hit), otherwise
 * establishes a new connection (miss).
 *
 * @param pool  Pool handle.
 * @param host  Remote hostname or IP.
 * @param port  Remote port.
 * @param conn  [out] Acquired connection.
 * @return DISTRIC_OK on success.
 *         DISTRIC_ERR_TIMEOUT if new connection timed out.
 *         DISTRIC_ERR_INIT_FAILED on connection failure.
 */
distric_err_t tcp_pool_acquire(
    tcp_pool_t*        pool,
    const char*        host,
    uint16_t           port,
    tcp_connection_t** conn
);

/**
 * @brief Release a connection back to the pool.
 *
 * If the pool is full, the connection is closed instead of pooled.
 * If the connection was marked failed, it is closed unconditionally.
 *
 * @param pool  Pool handle.
 * @param conn  Connection to release.
 */
void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn);

/**
 * @brief Mark a connection as failed so it will not be reused.
 *
 * Call this when an error is detected during use. The connection
 * will be closed when tcp_pool_release() is called.
 *
 * @param pool  Pool handle.
 * @param conn  Failed connection.
 */
void tcp_pool_mark_failed(tcp_pool_t* pool, tcp_connection_t* conn);

/**
 * @brief Get pool statistics.
 *
 * @param pool      Pool handle.
 * @param size_out  [out] Current idle connection count.
 * @param hits_out  [out] Cumulative cache hits.
 * @param misses_out [out] Cumulative cache misses.
 */
void tcp_pool_get_stats(
    tcp_pool_t* pool,
    size_t*     size_out,
    uint64_t*   hits_out,
    uint64_t*   misses_out
);

/**
 * @brief Destroy the pool and close all idle connections.
 *
 * @param pool  Pool to destroy (may be NULL).
 */
void tcp_pool_destroy(tcp_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_POOL_H */