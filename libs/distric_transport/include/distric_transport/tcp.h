/**
 * @file tcp.h
 * @brief TCP Server and Connection API for DistriC Transport Layer
 * 
 * Provides non-blocking, event-driven TCP server and client functionality
 * with integrated observability (metrics, logging, tracing).
 * 
 * Features:
 * - Non-blocking I/O using epoll (Linux) or kqueue (BSD/macOS)
 * - Connection lifecycle management
 * - Integrated metrics and structured logging
 * - Thread-safe operations
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_TRANSPORT_TCP_H
#define DISTRIC_TRANSPORT_TCP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

typedef struct tcp_server_s tcp_server_t;
typedef struct tcp_connection_s tcp_connection_t;

/* ============================================================================
 * CONNECTION CALLBACK
 * ========================================================================= */

/**
 * @brief Callback invoked when a new connection is accepted
 * 
 * @param conn The new connection object
 * @param userdata User-provided context data
 */
typedef void (*tcp_connection_callback_t)(tcp_connection_t* conn, void* userdata);

/* ============================================================================
 * TCP SERVER API
 * ========================================================================= */

/**
 * @brief Create a new TCP server
 * 
 * @param bind_addr Address to bind to (e.g., "0.0.0.0", "127.0.0.1")
 * @param port Port to listen on
 * @param metrics Metrics registry for observability
 * @param logger Logger instance for structured logging
 * @param server [out] Pointer to store created server
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t tcp_server_create(
    const char* bind_addr,
    uint16_t port,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_server_t** server
);

/**
 * @brief Start the TCP server and begin accepting connections
 * 
 * This function starts the event loop in a separate thread.
 * The provided callback will be invoked for each new connection.
 * 
 * @param server The server instance
 * @param on_connection Callback for new connections
 * @param userdata User context passed to callback
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t tcp_server_start(
    tcp_server_t* server,
    tcp_connection_callback_t on_connection,
    void* userdata
);

/**
 * @brief Stop the TCP server and close all connections
 * 
 * @param server The server instance
 */
distric_err_t tcp_server_stop(tcp_server_t* server);

/**
 * @brief Destroy the TCP server and free all resources
 * 
 * @param server The server instance
 */
void tcp_server_destroy(tcp_server_t* server);

/* ============================================================================
 * TCP CONNECTION API
 * ========================================================================= */

/**
 * @brief Send data over a TCP connection
 * 
 * This is a non-blocking send operation. If the send buffer is full,
 * it will return the number of bytes actually sent (may be less than len).
 * 
 * @param conn The connection
 * @param data Data buffer to send
 * @param len Number of bytes to send
 * @return Number of bytes sent on success, negative error code on failure
 */
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);

/**
 * @brief Receive data from a TCP connection
 * 
 * @param conn The connection
 * @param buffer Buffer to store received data
 * @param len Maximum number of bytes to receive
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking, -1 = infinite)
 * @return Number of bytes received on success, 0 on connection close, 
 *         negative error code on failure
 */
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);

/**
 * @brief Close a TCP connection
 * 
 * @param conn The connection to close
 */
void tcp_close(tcp_connection_t* conn);

/**
 * @brief Get the remote address of a connection
 * 
 * @param conn The connection
 * @param addr_out [out] Buffer to store address string
 * @param addr_len Size of address buffer
 * @param port_out [out] Pointer to store port number
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char* addr_out,
    size_t addr_len,
    uint16_t* port_out
);

/**
 * @brief Get connection ID (unique identifier)
 * 
 * @param conn The connection
 * @return Connection ID
 */
uint64_t tcp_get_connection_id(tcp_connection_t* conn);

/* ============================================================================
 * TCP CLIENT API
 * ========================================================================= */

/**
 * @brief Connect to a remote TCP server
 * 
 * @param host Hostname or IP address
 * @param port Port number
 * @param timeout_ms Connection timeout in milliseconds
 * @param metrics Metrics registry for observability
 * @param logger Logger instance
 * @param conn [out] Pointer to store created connection
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t tcp_connect(
    const char* host,
    uint16_t port,
    int timeout_ms,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_connection_t** conn
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_H */