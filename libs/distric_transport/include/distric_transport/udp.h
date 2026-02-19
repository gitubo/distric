/**
 * @file udp.h
 * @brief DistriC UDP Transport — Non-Blocking with Rate Limiting v4
 *
 * ==========================================================================
 * RATE LIMITING & PEER EVICTION (v4)
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
 *                    When this limit is reached, the LRU entry is evicted.
 *   peer_ttl_s       TTL in seconds for peer state. Peers not seen within
 *                    this window are preferentially evicted.
 *                    0 = 30 seconds.
 *
 * Memory ceiling:
 *   With max_peers capped, memory usage is O(max_peers) regardless of the
 *   number of distinct source IPs seen — DoS-safe.
 *
 * Eviction visibility (v4):
 *   udp_get_eviction_count() returns total LRU/TTL evictions since socket
 *   creation. A rising eviction counter under load indicates an active
 *   IP-spoofing flood that is consuming the peer tracking table.
 *   Also exported as metric: udp_peer_evictions_total.
 *
 * Eviction correctness (v4):
 *   evict_lru_peer() scans the full table (not stopping at hash-chain gaps)
 *   to guarantee the true LRU candidate is always found, even under heavy
 *   collision fragmentation from a flood.
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
 * @version 4.0.0
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
 * Metrics registered (if metrics != NULL):
 *   udp_packets_sent_total, udp_packets_recv_total,
 *   udp_bytes_sent_total,   udp_bytes_recv_total,
 *   udp_send_errors_total,  udp_recv_errors_total,
 *   udp_packets_dropped_total, udp_peer_evictions_total
 *
 * @param bind_addr   Address to bind.
 * @param port        Port to bind (0 = ephemeral).
 * @param rate_cfg    Rate limiting config (NULL = unlimited).
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
 * @return > 0                       Bytes sent.
 * @return DISTRIC_ERR_BACKPRESSURE  Kernel buffer full; retry later.
 * @return DISTRIC_ERR_IO            Send error.
 * @return DISTRIC_ERR_INVALID_ARG   NULL or bad address.
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
 * @param len         Buffer size.
 * @param src_addr    [out] Source address (min 64 bytes). May be NULL.
 * @param src_port    [out] Source port. May be NULL.
 * @param timeout_ms  -1 = non-blocking; 0 = infinite; >0 = bounded wait.
 *
 * @return > 0             Bytes received.
 * @return 0               No datagram (timeout, WOULD_BLOCK, rate-limited).
 * @return DISTRIC_ERR_IO  Receive error.
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
 *
 * Incremented for every rate-limited packet. Lock-free atomic read.
 */
uint64_t udp_get_drop_count(udp_socket_t* sock);

/**
 * @brief Return total peer table eviction count since socket creation.
 *
 * Incremented each time an LRU or TTL-expired peer is evicted from the
 * token-bucket table to make room for a new peer. A rising value under
 * flood conditions indicates the table is being saturated, likely by
 * IP-spoofed source addresses. Lock-free atomic read.
 */
uint64_t udp_get_eviction_count(udp_socket_t* sock);

/**
 * @brief Close and free the socket.
 *
 * Logs final drop and eviction counts if a logger was provided.
 */
void udp_close(udp_socket_t* sock);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_UDP_H */