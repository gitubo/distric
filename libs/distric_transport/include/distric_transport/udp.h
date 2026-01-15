/**
 * @file udp.h
 * @brief UDP Socket API
 * 
 * Fast, connectionless communication for gossip protocols and
 * other unreliable messaging patterns.
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_TRANSPORT_UDP_H
#define DISTRIC_TRANSPORT_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

typedef struct udp_socket_s udp_socket_t;

/* ============================================================================
 * UDP SOCKET API
 * ========================================================================= */

/**
 * @brief Create a new UDP socket
 * 
 * @param bind_addr Address to bind to (e.g., "0.0.0.0", "127.0.0.1")
 * @param port Port to bind to
 * @param metrics Metrics registry for observability
 * @param logger Logger instance
 * @param socket [out] Pointer to store created socket
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t udp_socket_create(
    const char* bind_addr,
    uint16_t port,
    metrics_registry_t* metrics,
    logger_t* logger,
    udp_socket_t** socket
);

/**
 * @brief Send a UDP datagram
 * 
 * @param socket The UDP socket
 * @param data Data buffer to send
 * @param len Number of bytes to send (max 65507 bytes)
 * @param dest_addr Destination address
 * @param dest_port Destination port
 * @return Number of bytes sent on success, negative error code on failure
 */
int udp_send(
    udp_socket_t* socket,
    const void* data,
    size_t len,
    const char* dest_addr,
    uint16_t dest_port
);

/**
 * @brief Receive a UDP datagram
 * 
 * @param socket The UDP socket
 * @param buffer Buffer to store received data
 * @param len Maximum number of bytes to receive
 * @param src_addr [out] Buffer to store source address (min 256 bytes)
 * @param src_port [out] Pointer to store source port
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking, -1 = infinite)
 * @return Number of bytes received on success, 0 on timeout, 
 *         negative error code on failure
 */
int udp_recv(
    udp_socket_t* socket,
    void* buffer,
    size_t len,
    char* src_addr,
    uint16_t* src_port,
    int timeout_ms
);

/**
 * @brief Close and destroy the UDP socket
 * 
 * @param socket The UDP socket
 */
void udp_close(udp_socket_t* socket);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_UDP_H */