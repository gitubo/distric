/**
 * @file distric_transport.h
 * @brief DistriC Transport Layer — Single Public Header v2
 *
 * This is the ONLY header that consumers of distric_transport need to include.
 *
 * Modules:
 *   - TCP server and client (non-blocking, epoll-based)
 *   - TCP connection pool (LRU eviction, thread-safe)
 *   - UDP socket (non-blocking, per-peer rate limiting)
 *   - Transport error taxonomy (stable classification of OS errors)
 *
 * ==========================================================================
 * QUICK START
 * ==========================================================================
 *
 * @code
 * #include <distric_transport.h>
 *
 * // Initialize observability (distric_obs)
 * metrics_registry_t* metrics;
 * logger_t*           logger;
 * metrics_init(&metrics);
 * log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
 *
 * // --- TCP SERVER ---
 * tcp_server_t* server;
 * tcp_server_create("0.0.0.0", 9000, metrics, logger, &server);
 * tcp_server_start(server, on_connection_callback, NULL);
 *
 * // --- TCP CLIENT with backpressure ---
 * tcp_connection_t* conn;
 * tcp_connect("10.0.1.5", 9000, 5000, NULL, metrics, logger, &conn);
 *
 * int rc = tcp_send(conn, data, len);
 * if (rc == DISTRIC_ERR_BACKPRESSURE) {
 *     // Stop sending; call tcp_flush() later or close the connection
 * }
 *
 * // --- TCP POOL ---
 * tcp_pool_t* pool;
 * tcp_pool_create(100, metrics, logger, &pool);
 * tcp_pool_acquire(pool, "10.0.1.5", 9000, &conn);
 * tcp_send(conn, data, len);
 * tcp_pool_release(pool, conn);
 *
 * // --- UDP with rate limiting ---
 * udp_rate_limit_config_t rl = { .rate_limit_pps = 1000, .burst_size = 2000 };
 * udp_socket_t* udp;
 * udp_socket_create("0.0.0.0", 9001, &rl, metrics, logger, &udp);
 * udp_send(udp, data, len, "10.0.1.6", 9001);
 *
 * // --- Cleanup (reverse order) ---
 * udp_close(udp);
 * tcp_pool_destroy(pool);
 * tcp_close(conn);
 * tcp_server_destroy(server);
 * log_destroy(logger);
 * metrics_destroy(metrics);
 * @endcode
 *
 * @version 2.0.0
 */

#ifndef DISTRIC_TRANSPORT_H
#define DISTRIC_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ERROR TAXONOMY (include first — other headers depend on it)
 * ========================================================================= */

/**
 * @defgroup transport_error Transport Error Taxonomy
 * @brief Stable classification of OS errors into transport categories.
 * @{
 */
#include "distric_transport/transport_error.h"
/** @} */

/* ============================================================================
 * TCP TRANSPORT
 * ========================================================================= */

/**
 * @defgroup tcp TCP Transport
 * @brief Non-blocking, epoll-based TCP server and client with backpressure.
 *
 * Key guarantees:
 *  - All sockets are O_NONBLOCK; no library call blocks indefinitely.
 *  - tcp_send() returns DISTRIC_ERR_BACKPRESSURE when the per-connection
 *    send queue exceeds its high-water mark.
 *  - Observability (logging, metrics) is not in the send/recv hot path.
 * @{
 */
#include "distric_transport/tcp.h"
/** @} */

/* ============================================================================
 * TCP CONNECTION POOL
 * ========================================================================= */

/**
 * @defgroup tcp_pool TCP Connection Pool
 * @brief Thread-safe connection reuse with LRU eviction.
 * @{
 */
#include "distric_transport/tcp_pool.h"
/** @} */

/* ============================================================================
 * UDP TRANSPORT
 * ========================================================================= */

/**
 * @defgroup udp UDP Transport
 * @brief Non-blocking UDP with per-peer token-bucket rate limiting.
 *
 * Rate limiting protects against gossip storms:
 *  - Configure via udp_rate_limit_config_t on socket creation.
 *  - Excess packets are silently dropped (counter incremented).
 *  - Drop count readable via udp_get_drop_count().
 * @{
 */
#include "distric_transport/udp.h"
/** @} */

/* ============================================================================
 * VERSION
 * ========================================================================= */

#define DISTRIC_TRANSPORT_VERSION_MAJOR 2
#define DISTRIC_TRANSPORT_VERSION_MINOR 0
#define DISTRIC_TRANSPORT_VERSION_PATCH 0

static inline const char* distric_transport_version(void) {
    return "2.0.0";
}

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_H */