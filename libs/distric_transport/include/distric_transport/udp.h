/**
 * @file udp.h
 * @brief DistriC UDP Transport — Non-Blocking with Rate Limiting v2
 *
 * ==========================================================================
 * RATE LIMITING
 * ==========================================================================
 *
 * UDP is connectionless and unbounded by design. Without protection, gossip
 * storms and misbehaving peers can saturate the receive thread and overwhelm
 * the kernel socket buffer. This module implements per-peer token-bucket
 * rate limiting on the receive path.
 *
 * Configuration:
 *   rate_limit_pps   Packets per second permitted from a single source IP.
 *                    0 = unlimited (default for trusted environments).
 *   burst_size       Maximum burst (tokens) allowed above the steady rate.
 *                    Typical: 2× rate_limit_pps.
 *
 * Dropped packets:
 *   - A drop counter is incremented atomically (no lock in hot path).
 *   - Drops are exported via the "udp_packets_dropped_total" metric.
 *   - No log entry is emitted per drop (to avoid log amplification under
 *     attack); a periodic summary is logged instead.
 *
 * ==========================================================================
 * I/O MODEL
 * ==========================================================================
 *
 *   udp_send():   Non-blocking. Returns immediately on EAGAIN (UDP sockets
 *                 rarely block, but the buffer can fill under load).
 *   udp_recv():   timeout_ms controls whether the call blocks:
 *                   -1  Non-blocking (returns 0 if no datagram).
 *                    0  Wait indefinitely.
 *                   >0  Wait up to timeout_ms milliseconds.
 *
 * @version 2.0.0
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

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

typedef struct udp_socket_s udp_socket_t;

/* ============================================================================
 * RATE LIMIT CONFIGURATION
 * ========================================================================= */

/**
 * @brief Per-peer token-bucket rate limit configuration.
 *
 * Zero-initialise to disable rate limiting (all packets pass).
 */
typedef struct {
    uint32_t rate_limit_pps;  /**< Packets/second per source IP. 0 = unlimited. */
    uint32_t burst_size;      /**< Max burst tokens. 0 = 2× rate_limit_pps.     */
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
 * @param rate_cfg    Rate limiting config (NULL = unlimited).
 * @param metrics     Metrics registry (NULL = no metrics).
 * @param logger      Logger instance (NULL = no logging).
 * @param sock_out    [out] Created socket handle.
 * @return DISTRIC_OK on success, error code otherwise.
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
 * Non-blocking. If the kernel send buffer is full (EAGAIN), the call
 * returns DISTRIC_ERR_BACKPRESSURE immediately without blocking.
 *
 * @param sock       UDP socket.
 * @param data       Data to send.
 * @param len        Datagram size in bytes (max 65507).
 * @param dest_addr  Destination address string.
 * @param dest_port  Destination port.
 *
 * @return > 0  Bytes sent (equals len on success for UDP).
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
 * Applies per-peer rate limiting if configured. Rate-limited packets are
 * silently dropped (counter incremented) and the call returns 0 as if no
 * data was available.
 *
 * @param sock        UDP socket.
 * @param buffer      Receive buffer.
 * @param len         Buffer size in bytes.
 * @param src_addr    [out] Source address string (min 64 bytes). May be NULL.
 * @param src_port    [out] Source port. May be NULL.
 * @param timeout_ms  -1 = non-blocking; 0 = infinite wait; >0 = timeout.
 *
 * @return > 0  Bytes received.
 * @return 0    No datagram (timeout, WOULD_BLOCK, or rate-limited drop).
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
 * @brief Return the number of packets dropped due to rate limiting.
 *
 * Thread-safe (atomic read).
 *
 * @param sock  UDP socket.
 * @return Total dropped packet count since socket creation.
 */
uint64_t udp_get_drop_count(udp_socket_t* sock);

/**
 * @brief Close and free the UDP socket.
 *
 * @param sock  Socket to close (may be NULL).
 */
void udp_close(udp_socket_t* sock);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_UDP_H */