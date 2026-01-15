/**
 * @file distric_transport.h
 * @brief DistriC Transport Layer - Single Public Header
 * 
 * This is the ONLY header file that users of the distric_transport library
 * need to include. It aggregates all transport functionality:
 * - TCP server and client
 * - TCP connection pooling
 * - UDP sockets
 * 
 * All transport components integrate with distric_obs for metrics, logging,
 * and distributed tracing.
 * 
 * Usage:
 * @code
 * #include <distric_transport.h>
 * 
 * // Initialize observability first
 * metrics_registry_t* metrics;
 * logger_t* logger;
 * metrics_init(&metrics);
 * log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
 * 
 * // Create TCP server
 * tcp_server_t* server;
 * tcp_server_create("0.0.0.0", 9000, metrics, logger, &server);
 * tcp_server_start(server, on_connection_callback, NULL);
 * 
 * // Create connection pool
 * tcp_pool_t* pool;
 * tcp_pool_create(100, metrics, logger, &pool);
 * 
 * // Connect to remote server
 * tcp_connection_t* conn;
 * tcp_pool_acquire(pool, "10.0.1.5", 9000, &conn);
 * tcp_send(conn, data, len);
 * tcp_pool_release(pool, conn);
 * 
 * // Create UDP socket
 * udp_socket_t* udp;
 * udp_socket_create("0.0.0.0", 9001, metrics, logger, &udp);
 * udp_send(udp, data, len, "10.0.1.6", 9001);
 * 
 * // Cleanup
 * udp_close(udp);
 * tcp_pool_destroy(pool);
 * tcp_server_destroy(server);
 * log_destroy(logger);
 * metrics_destroy(metrics);
 * @endcode
 * 
 * @version 1.0.0
 * @author DistriC Development Team
 */

#ifndef DISTRIC_TRANSPORT_H
#define DISTRIC_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TRANSPORT LAYER MODULES
 * ========================================================================= */

/**
 * @defgroup tcp TCP Transport
 * @brief Reliable, connection-oriented communication
 * 
 * Provides non-blocking TCP server and client with integrated observability.
 * - Event-driven I/O (epoll on Linux, kqueue on BSD/macOS)
 * - Connection lifecycle management
 * - Automatic metrics and logging
 * @{
 */
#include "distric_transport/tcp.h"
/** @} */

/**
 * @defgroup tcp_pool TCP Connection Pool
 * @brief Connection reuse for reduced handshake overhead
 * 
 * Thread-safe connection pooling with configurable limits.
 * - Automatic connection reuse
 * - LRU eviction policy
 * - Pool statistics (hits, misses, size)
 * @{
 */
#include "distric_transport/tcp_pool.h"
/** @} */

/**
 * @defgroup udp UDP Transport
 * @brief Fast, connectionless communication
 * 
 * Non-blocking UDP socket for gossip protocols and unreliable messaging.
 * - Datagram send/receive
 * - Timeout support
 * - Automatic metrics and logging
 * @{
 */
#include "distric_transport/udp.h"
/** @} */

/* ============================================================================
 * VERSION INFORMATION
 * ========================================================================= */

#define DISTRIC_TRANSPORT_VERSION_MAJOR 1
#define DISTRIC_TRANSPORT_VERSION_MINOR 0
#define DISTRIC_TRANSPORT_VERSION_PATCH 0

/**
 * @brief Get library version string
 * 
 * @return Version string (e.g., "1.0.0")
 */
static inline const char* distric_transport_version(void) {
    return "1.0.0";
}

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_H */