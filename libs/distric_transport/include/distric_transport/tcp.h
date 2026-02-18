/**
 * @file tcp.h
 * @brief DistriC TCP Transport — Non-Blocking API v2
 *
 * All sockets are strictly non-blocking (O_NONBLOCK). The library never
 * sleeps inside a send or receive call.
 *
 * ==========================================================================
 * I/O MODEL
 * ==========================================================================
 *
 * SEND
 *   tcp_send() attempts a direct kernel send. On EAGAIN the data is buffered
 *   in a per-connection send queue. The next tcp_send() flushes the queue
 *   before appending new data. If the queue grows above its high-water mark
 *   (HWM), tcp_send() returns DISTRIC_ERR_BACKPRESSURE — the caller MUST
 *   slow down. No data is silently dropped.
 *
 * RECEIVE
 *   tcp_recv() is non-blocking with an optional timeout. With timeout_ms = -1
 *   it returns immediately (WOULD_BLOCK → returns 0). With timeout_ms >= 0 it
 *   waits up to that many milliseconds using a single epoll_wait, then reads.
 *   It never blocks the calling thread beyond the specified timeout.
 *
 * ==========================================================================
 * BACKPRESSURE SIGNALS
 * ==========================================================================
 *
 *  tcp_send() returns DISTRIC_ERR_BACKPRESSURE  → queue >= HWM, stop sending.
 *  tcp_is_writable()  returns false             → equivalent check.
 *
 *  After a BACKPRESSURE return the caller should:
 *    1. Stop sending new data.
 *    2. Call tcp_send() again later; it will flush the queue automatically.
 *    3. On persistent backpressure, drop the connection (tcp_close).
 *
 * ==========================================================================
 * ERROR HANDLING
 * ==========================================================================
 *
 *  All I/O functions return negative distric_err_t codes on hard failure.
 *  Use transport_classify_errno() / transport_err_str() for details.
 *
 * @version 2.0.0
 */

#ifndef DISTRIC_TRANSPORT_TCP_H
#define DISTRIC_TRANSPORT_TCP_H

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

typedef struct tcp_server_s     tcp_server_t;
typedef struct tcp_connection_s tcp_connection_t;

/* ============================================================================
 * CONNECTION CONFIGURATION
 * ========================================================================= */

/**
 * @brief Per-connection configuration.
 *
 * Zero-initialise to get safe defaults.
 */
typedef struct {
    /**
     * Total send-queue capacity in bytes.
     * 0 = use SEND_QUEUE_DEFAULT_CAPACITY (64 KB).
     */
    size_t send_queue_capacity;

    /**
     * High-water mark in bytes. When the queue exceeds this value,
     * tcp_send() returns DISTRIC_ERR_BACKPRESSURE.
     * 0 = use SEND_QUEUE_DEFAULT_HWM (48 KB = 75%).
     */
    size_t send_queue_hwm;
} tcp_connection_config_t;

/* ============================================================================
 * CONNECTION CALLBACK
 * ========================================================================= */

/**
 * @brief Callback invoked when the server accepts a new connection.
 *
 * The callback owns the connection; it MUST call tcp_close() when done.
 *
 * @param conn      Accepted connection.
 * @param userdata  Caller-supplied context.
 */
typedef void (*tcp_connection_callback_t)(tcp_connection_t* conn, void* userdata);

/* ============================================================================
 * TCP SERVER API
 * ========================================================================= */

/**
 * @brief Create a new TCP server.
 *
 * The server socket is created and bound but not yet listening. Call
 * tcp_server_start() to begin accepting connections.
 *
 * @param bind_addr  Address to bind (e.g., "0.0.0.0", "127.0.0.1").
 * @param port       Port to listen on.
 * @param metrics    Metrics registry (may be NULL to disable metrics).
 * @param logger     Logger instance (may be NULL to disable logging).
 * @param server     [out] Created server handle.
 * @return DISTRIC_OK on success, error code otherwise.
 */
distric_err_t tcp_server_create(
    const char*         bind_addr,
    uint16_t            port,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_server_t**      server
);

/**
 * @brief Start accepting connections in a background thread.
 *
 * The accept loop runs on a dedicated thread using epoll. Each accepted
 * connection is dispatched to @p on_connection in its own detached thread.
 *
 * @param server         Server to start.
 * @param on_connection  Callback for each accepted connection.
 * @param userdata       Context passed to the callback.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_server_start(
    tcp_server_t*             server,
    tcp_connection_callback_t on_connection,
    void*                     userdata
);

/**
 * @brief Stop accepting new connections and wait for the accept thread to exit.
 *
 * Does NOT close active connections; those are owned by their callbacks.
 *
 * @param server  Server to stop.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_server_stop(tcp_server_t* server);

/**
 * @brief Stop (if running) and free all server resources.
 *
 * @param server  Server to destroy (may be NULL).
 */
void tcp_server_destroy(tcp_server_t* server);

/* ============================================================================
 * TCP CONNECTION API
 * ========================================================================= */

/**
 * @brief Connect to a remote TCP server.
 *
 * Uses a non-blocking connect with epoll-based timeout.
 *
 * @param host        Hostname or IP address (resolved via getaddrinfo).
 * @param port        Port number.
 * @param timeout_ms  Connection timeout in milliseconds (>0 required).
 * @param config      Connection configuration (NULL = defaults).
 * @param metrics     Metrics registry (may be NULL).
 * @param logger      Logger instance (may be NULL).
 * @param conn        [out] Created connection handle.
 * @return DISTRIC_OK on success.
 *         DISTRIC_ERR_TIMEOUT if connection did not complete in time.
 *         DISTRIC_ERR_INIT_FAILED on OS error.
 */
distric_err_t tcp_connect(
    const char*                    host,
    uint16_t                       port,
    int                            timeout_ms,
    const tcp_connection_config_t* config,
    metrics_registry_t*            metrics,
    logger_t*                      logger,
    tcp_connection_t**             conn
);

/**
 * @brief Send data over a connection.
 *
 * Non-blocking. Attempts a direct kernel send; on EAGAIN, buffers data in
 * the per-connection send queue.
 *
 * @param conn  Connection handle.
 * @param data  Data buffer.
 * @param len   Number of bytes to send.
 *
 * @return > 0  Number of bytes accepted (sent + queued). May equal len.
 * @return DISTRIC_ERR_BACKPRESSURE  Queue above HWM; caller must slow down.
 * @return DISTRIC_ERR_IO            Hard send error (connection is broken).
 * @return DISTRIC_ERR_INVALID_ARG   NULL pointer or zero length.
 */
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);

/**
 * @brief Receive data from a connection.
 *
 * @param conn        Connection handle.
 * @param buffer      Receive buffer.
 * @param len         Buffer size in bytes.
 * @param timeout_ms  -1 = non-blocking (immediate return).
 *                    0  = infinite wait (blocks until data or error).
 *                    >0 = wait up to this many milliseconds.
 *
 * @return > 0  Number of bytes received.
 * @return 0    No data available (timeout or WOULD_BLOCK).
 * @return DISTRIC_ERR_EOF  Peer closed the connection cleanly.
 * @return DISTRIC_ERR_IO   Receive error.
 */
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);

/**
 * @brief Flush the pending send queue without adding new data.
 *
 * Useful after receiving a BACKPRESSURE error to drain the queue.
 *
 * @param conn  Connection handle.
 * @return DISTRIC_OK if queue is now empty.
 *         DISTRIC_ERR_BACKPRESSURE if queue is still above HWM.
 *         DISTRIC_ERR_IO on hard send error.
 */
distric_err_t tcp_flush(tcp_connection_t* conn);

/**
 * @brief Return true if the send queue is below the high-water mark.
 *
 * Callers can poll this before calling tcp_send() to avoid a BACKPRESSURE
 * return.
 *
 * @param conn  Connection handle.
 * @return true if writable (queue < HWM), false if backpressure applies.
 */
bool tcp_is_writable(tcp_connection_t* conn);

/**
 * @brief Return the number of bytes currently pending in the send queue.
 *
 * @param conn  Connection handle.
 * @return Pending bytes, or 0 on NULL input.
 */
size_t tcp_send_queue_depth(tcp_connection_t* conn);

/**
 * @brief Close and free a connection.
 *
 * Safe to call with NULL. Pending send queue data is discarded.
 *
 * @param conn  Connection to close.
 */
void tcp_close(tcp_connection_t* conn);

/**
 * @brief Get the remote address and port.
 *
 * @param conn      Connection handle.
 * @param addr_out  Buffer to store address string (min 64 bytes recommended).
 * @param addr_len  Size of addr_out.
 * @param port_out  [out] Remote port.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char*             addr_out,
    size_t            addr_len,
    uint16_t*         port_out
);

/**
 * @brief Return the unique connection identifier.
 *
 * IDs are monotonically increasing per process. Server-accepted connections
 * and client connections share the same counter.
 *
 * @param conn  Connection handle.
 * @return Connection ID, or 0 on NULL input.
 */
uint64_t tcp_get_connection_id(tcp_connection_t* conn);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_H */