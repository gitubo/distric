/**
 * @file tcp.h
 * @brief DistriC TCP Transport — Non-Blocking API v3
 *
 * All sockets are strictly non-blocking (O_NONBLOCK). The library never
 * sleeps inside a send or receive call.
 *
 * ==========================================================================
 * CONCURRENCY MODEL (v3)
 * ==========================================================================
 *
 * The server uses a BOUNDED WORKER POOL instead of unbounded detached threads.
 *
 *   Accept thread  → accepts connections, submits to work queue
 *   Worker pool    → N threads (default: CPU count) process callbacks
 *   Work queue     → bounded MPSC ring; drops + logs when full (back-pressure)
 *
 * Benefits:
 *   - Thread count is bounded to O(CPU count), not O(connections).
 *   - No thread explosion under bursty accept rates.
 *   - Configurable pool size via tcp_server_config_t.worker_threads.
 *
 * ==========================================================================
 * GRACEFUL SHUTDOWN (v3)
 * ==========================================================================
 *
 *   RUNNING → (tcp_server_stop) → DRAINING → (active == 0 or timeout) → STOPPED
 *
 *   In DRAINING state:
 *     - No new connections are accepted.
 *     - Active connections continue until their callbacks return.
 *     - tcp_server_stop() blocks until drain completes or drain_timeout_ms.
 *
 * ==========================================================================
 * I/O MODEL
 * ==========================================================================
 *
 * SEND
 *   tcp_send() attempts a direct kernel send. On EAGAIN the data is buffered
 *   in a per-connection ring-buffer send queue. When the queue grows above
 *   its high-water mark (HWM), tcp_send() returns DISTRIC_ERR_BACKPRESSURE.
 *
 * RECEIVE
 *   tcp_recv() is non-blocking with an optional timeout. With timeout_ms = -1
 *   it returns immediately. With timeout_ms >= 0 it waits using a cached
 *   per-connection epoll fd (not a temporary one-shot allocation).
 *
 * ==========================================================================
 * CIRCUIT BREAKER (v3)
 * ==========================================================================
 *
 *   tcp_connect() checks a per-server circuit breaker before dialing.
 *   Failures transition the breaker toward OPEN, rejecting all attempts
 *   until a recovery timeout expires and a probe succeeds.
 *   See circuit_breaker.h for full state machine details.
 *
 * @version 3.0.0
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
 * SERVER STATE
 * ========================================================================= */

/**
 * @brief Graceful shutdown state machine states.
 */
typedef enum {
    TCP_SERVER_RUNNING  = 0,  /**< Accepting and processing connections.    */
    TCP_SERVER_DRAINING = 1,  /**< Not accepting; waiting for active to 0.  */
    TCP_SERVER_STOPPED  = 2,  /**< Fully stopped; safe to destroy.          */
} tcp_server_state_t;

/* ============================================================================
 * SERVER CONFIGURATION
 * ========================================================================= */

/**
 * @brief Server creation configuration.
 *
 * Zero-initialise to use safe defaults.
 */
typedef struct {
    /**
     * Number of worker threads in the connection handler pool.
     * 0 = auto-detect (number of logical CPUs, capped at 32).
     */
    uint32_t worker_threads;

    /**
     * Maximum pending connections in the worker dispatch queue.
     * 0 = 4096. When full, new connections are rejected (closed immediately).
     */
    uint32_t worker_queue_depth;

    /**
     * Milliseconds to wait for active connections to finish during drain.
     * 0 = 5000 ms.
     */
    uint32_t drain_timeout_ms;

    /**
     * Per-connection send queue and HWM configuration applied to all
     * connections accepted by this server.
     */
    size_t conn_send_queue_capacity;
    size_t conn_send_queue_hwm;
} tcp_server_config_t;

/* ============================================================================
 * CONNECTION CONFIGURATION
 * ========================================================================= */

/**
 * @brief Per-connection configuration for outbound connections (tcp_connect).
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
 * @brief Callback invoked when the server dispatches a new connection.
 *
 * Executed in a worker thread. The callback owns the connection and MUST
 * call tcp_close() before returning.
 *
 * @param conn      Accepted connection.
 * @param userdata  Caller-supplied context.
 */
typedef void (*tcp_connection_callback_t)(tcp_connection_t* conn, void* userdata);

/* ============================================================================
 * TCP SERVER API
 * ========================================================================= */

/**
 * @brief Create a new TCP server with default configuration.
 *
 * @param bind_addr  Address to bind (e.g., "0.0.0.0", "127.0.0.1").
 * @param port       Port to listen on.
 * @param metrics    Metrics registry (may be NULL).
 * @param logger     Logger instance (may be NULL).
 * @param server     [out] Created server handle.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_server_create(
    const char*         bind_addr,
    uint16_t            port,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_server_t**      server
);

/**
 * @brief Create a new TCP server with explicit configuration.
 *
 * @param bind_addr  Address to bind.
 * @param port       Port to listen on.
 * @param cfg        Server configuration (NULL = defaults).
 * @param metrics    Metrics registry (may be NULL).
 * @param logger     Logger instance (may be NULL).
 * @param server     [out] Created server handle.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_server_create_with_config(
    const char*              bind_addr,
    uint16_t                 port,
    const tcp_server_config_t* cfg,
    metrics_registry_t*      metrics,
    logger_t*                logger,
    tcp_server_t**           server
);

/**
 * @brief Start accepting connections using the worker pool.
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
 * @brief Transition to DRAINING and wait for active connections to finish.
 *
 * Blocks until all active connections complete or drain_timeout_ms expires.
 * After this call, state is TCP_SERVER_STOPPED.
 *
 * @param server  Server to stop.
 * @return DISTRIC_OK on success.
 */
distric_err_t tcp_server_stop(tcp_server_t* server);

/**
 * @brief Return the current server state.
 */
tcp_server_state_t tcp_server_get_state(const tcp_server_t* server);

/**
 * @brief Return the number of currently active connections.
 */
int64_t tcp_server_active_connections(const tcp_server_t* server);

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
 * Checks the circuit breaker for host before dialing. Records success/failure
 * to maintain per-host breaker state.
 *
 * @param host        Hostname or IP address.
 * @param port        Port number.
 * @param timeout_ms  Connection timeout in milliseconds (>0 required).
 * @param config      Connection configuration (NULL = defaults).
 * @param metrics     Metrics registry (may be NULL).
 * @param logger      Logger instance (may be NULL).
 * @param conn        [out] Created connection handle.
 * @return DISTRIC_OK on success.
 *         DISTRIC_ERR_TIMEOUT if connection did not complete in time.
 *         DISTRIC_ERR_UNAVAILABLE if circuit breaker is OPEN (host is failing).
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
 * Non-blocking. Buffers into the per-connection ring-buffer send queue on
 * EAGAIN. Returns DISTRIC_ERR_BACKPRESSURE when queue >= HWM.
 *
 * @return > 0                    Bytes accepted (sent + queued).
 * @return DISTRIC_ERR_BACKPRESSURE  Queue at HWM; caller must throttle.
 * @return DISTRIC_ERR_IO            Hard send error.
 * @return DISTRIC_ERR_INVALID_ARG   NULL or zero length.
 */
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);

/**
 * @brief Receive data from a connection.
 *
 * Uses a cached per-connection epoll fd for readiness detection (no
 * allocation per call).
 *
 * @param conn        Connection handle.
 * @param buffer      Receive buffer.
 * @param len         Buffer size in bytes.
 * @param timeout_ms  -1 = non-blocking; 0 = infinite; >0 = bounded wait.
 *
 * @return > 0               Bytes received.
 * @return 0                 No data (timeout or WOULD_BLOCK).
 * @return DISTRIC_ERR_EOF   Peer closed the connection cleanly.
 * @return DISTRIC_ERR_IO    Receive error.
 */
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);

/**
 * @brief Return pending bytes in the send queue.
 */
size_t tcp_send_queue_depth(const tcp_connection_t* conn);

/**
 * @brief Return true if the send queue is below the HWM.
 */
bool tcp_is_writable(const tcp_connection_t* conn);

/**
 * @brief Return the remote address string.
 */
distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char* addr_out, size_t addr_len,
    uint16_t* port_out
);

/**
 * @brief Return the unique connection ID.
 */
uint64_t tcp_get_connection_id(tcp_connection_t* conn);

/**
 * @brief Close and free the connection.
 *
 * After this call @p conn is invalid.
 */
void tcp_close(tcp_connection_t* conn);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_H */