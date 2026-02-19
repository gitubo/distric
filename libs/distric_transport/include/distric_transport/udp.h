/**
 * @file udp.h
 * @brief DistriC UDP Transport — Non-Blocking with Rate Limiting v3
 *
 * ==========================================================================
 * RATE LIMITING & PEER EVICTION (v3)
 * ==========================================================================
 *
 * UDP is connectionless and attack-prone. This module implements per-peer
 * token-bucket rate limiting with LRU-based memory-bounded peer tracking.
 *
 * Configuration:
 *   rate_limit_pps   Packets per second permitted from a single source IP.
 *                    0 = unlimited.
 *   burst_size       Maximum burst tokens allowed above the steady rate.
 *                    0 = 2× rate_limit_pps.
 *   max_peers        Maximum number of distinct source IPs tracked.
 *                    0 = 256 (BUCKET_TABLE_SIZE).
 *                    When this limit is reached, the peer with the oldest
 *                    last-seen timestamp is evicted (LRU eviction).
 *   peer_ttl_s       TTL in seconds for peer state. Peers not seen within
 *                    this window are preferentially evicted.
 *                    0 = 30 seconds.
 *
 * Memory ceiling:
 *   With max_peers capped, memory usage is O(max_peers) regardless of the
 *   number of distinct source IPs seen (DoS-safe).
 *
 * ==========================================================================
 * I/O MODEL
 * ==========================================================================
 *
 *   udp_send():   Non-blocking. Returns DISTRIC_ERR_BACKPRESSURE on EAGAIN.
 *   udp_recv():   timeout_ms controls blocking:
 *                   -1  Non-blocking (returns 0 if no datagram).
 *                    0  Wait indefinitely.
 *                   >0  Wait up to timeout_ms milliseconds.
 *
 * @version 3.0.0
 */

#ifndef DISTRIC_TRANSPORT_UDP_H
#define DISTRIC_TRANSPORT_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <distric_obs.h>
#include "transport_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct udp_socket_s udp_socket_t;

/* ============================================================================
 * RATE LIMIT CONFIGURATION
 * ========================================================================= */

/**
 * @brief Per-peer token-bucket rate limit and eviction configuration.
 *
 * Zero-initialise to disable rate limiting (all packets pass).
 */
typedef struct {
    uint32_t rate_limit_pps;  /**< Packets/second per source IP. 0 = unlimited.     */
    uint32_t burst_size;      /**< Max burst tokens.  0 = 2× rate_limit_pps.        */
    uint32_t max_peers;       /**< Max tracked peers. 0 = 256. LRU eviction on full.*/
    uint32_t peer_ttl_s;      /**< Peer TTL in seconds. 0 = 30s.                    */
} udp_rate_limit_config_t;

/* ============================================================================
 * UDP SOCKET API
 * ========================================================================= */

/**
 * @brief Create and bind a UDP socket.
 *
 * The socket is created in non-blocking mode. Pass port = 0 to let the OS
 * assign an ephemeral port (sender-only use case).
 *
 * @param bind_addr   Address to bind (e.g., "0.0.0.0", "127.0.0.1").
 * @param port        Port to bind (0 = ephemeral).
 * @param rate_cfg    Rate limiting config (NULL = unlimited, no peer tracking).
 * @param metrics     Metrics registry (NULL = no metrics).
 * @param logger      Logger instance (NULL = no logging).
 * @param sock_out    [out] Created socket handle.
 * @return DISTRIC_OK on success.
 */
distric_err_t udp_socket_create(
    const char*                    bind_addr,
    uint16_t                       port,
    const udp_rate_limit_config_t* rate_cfg,
    metrics_registry_t*            metrics,
    logger_t*                      logger,
    udp_socket_t**                 sock_out
);

/**
 * @brief Send a UDP datagram.
 *
 * @param sock       UDP socket.
 * @param data       Data to send.
 * @param len        Datagram size in bytes (max 65507).
 * @param dest_addr  Destination address string.
 * @param dest_port  Destination port.
 *
 * @return > 0                       Bytes sent.
 * @return DISTRIC_ERR_BACKPRESSURE  Kernel buffer full; retry later.
 * @return DISTRIC_ERR_IO            Send error.
 * @return DISTRIC_ERR_INVALID_ARG   NULL/bad input.
 */
int udp_send(
    udp_socket_t* sock,
    const void*   data,
    size_t        len,
    const char*   dest_addr,
    uint16_t      dest_port
);

/**
 * @brief Receive a UDP datagram.
 *
 * Applies per-peer rate limiting with LRU eviction if configured.
 * Rate-limited packets are silently dropped (counter incremented).
 *
 * @param sock        UDP socket.
 * @param buffer      Receive buffer.
 * @param len         Buffer size in bytes.
 * @param src_addr    [out] Source address (min 64 bytes). May be NULL.
 * @param src_port    [out] Source port. May be NULL.
 * @param timeout_ms  -1 = non-blocking; 0 = infinite wait; >0 = timeout.
 *
 * @return > 0            Bytes received.
 * @return 0              No datagram (timeout, WOULD_BLOCK, or rate-limited).
 * @return DISTRIC_ERR_IO Receive error.
 */
int udp_recv(
    udp_socket_t* sock,
    void*         buffer,
    size_t        len,
    char*         src_addr,
    uint16_t*     src_port,
    int           timeout_ms
);

/**
 * @brief Return total dropped packet count since socket creation.
 */
uint64_t udp_get_drop_count(udp_socket_t* sock);

/**
 * @brief Close and free the socket.
 */
void udp_close(udp_socket_t* sock);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_UDP_H */