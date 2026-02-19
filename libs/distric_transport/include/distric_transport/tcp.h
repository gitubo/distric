/**
 * @file tcp.h
 * @brief DistriC TCP Transport — Non-Blocking API v4
 *
 * All sockets are strictly non-blocking (O_NONBLOCK). The library never
 * sleeps inside a send or receive call.
 *
 * ==========================================================================
 * CONCURRENCY MODEL (v4)
 * ==========================================================================
 *
 * The server uses a BOUNDED WORKER POOL instead of unbounded detached threads.
 *
 *   Accept thread  → accepts connections, submits to work queue
 *   Worker pool    → N threads (default: CPU count) process callbacks
 *   Work queue     → bounded, cache-line-padded MPSC ring (false-sharing free)
 *
 * Benefits:
 *   - Thread count is bounded to O(CPU count), not O(connections).
 *   - No thread explosion under bursty accept rates.
 *   - Configurable pool size via tcp_server_config_t.worker_threads.
 *   - Producer (head) and consumer (tail) indices on separate cache lines.
 *
 * ==========================================================================
 * ACCEPT BACKPRESSURE (v4)
 * ==========================================================================
 *
 *   When the worker queue saturation reaches TCP_QUEUE_PAUSE_PCT% (90%),
 *   the accept thread disables EPOLLIN on the listen fd. The kernel SYN
 *   backlog absorbs bursts without CPU waste. EPOLLIN is re-enabled when
 *   saturation drops below TCP_QUEUE_RESUME_PCT% (70%).
 *
 *   Metric exported: tcp_server_accept_rejections_total
 *
 * ==========================================================================
 * GRACEFUL SHUTDOWN (v4)
 * ==========================================================================
 *
 *   RUNNING → (tcp_server_stop) → DRAINING → (active == 0 or timeout) → STOPPED
 *
 *   In DRAINING state:
 *     - No new connections are accepted.
 *     - Workers wait on drain_cond (condvar) — no nanosleep polling.
 *     - On drain_timeout_ms expiry, wq_force_stop() is called: workers
 *       exit immediately and remaining queued connections are force-closed.
 *     - All worker threads are joined before tcp_server_stop() returns.
 *
 * ==========================================================================
 * I/O MODEL (v4)
 * ==========================================================================
 *
 * SEND
 *   tcp_send() attempts a direct kernel send. On EAGAIN the data is buffered
 *   in a per-connection ring-buffer send queue. When the queue grows above
 *   its high-water mark (HWM), tcp_send() returns DISTRIC_ERR_BACKPRESSURE.
 *
 * RECEIVE
 *   tcp_recv() is strictly non-blocking with timeout_ms = -1 (preferred).
 *   The timeout_ms parameter is DEPRECATED; callers should instead use:
 *     - tcp_is_readable() for a zero-cost readiness poll.
 *     - Their own epoll loop with tcp_connection_get_fd() (future extension).
 *   The helper tcp_is_readable() uses the cached per-connection epoll fd.
 *
 * ==========================================================================
 * OBSERVABILITY LIFECYCLE (v4)
 * ==========================================================================
 *
 *   Logger and metrics are stored as atomic pointers inside tcp_server_s.
 *   They are nullified with a seq_cst barrier AFTER accept thread join and
 *   BEFORE worker join, preventing any use-after-free in observability paths.
 *
 * ==========================================================================
 * CIRCUIT BREAKER (v3+)
 * ==========================================================================
 *
 *   tcp_connect() checks a module-level circuit breaker before dialing.
 *   The breaker uses a reader-writer lock: CLOSED path uses rdlock (fast),
 *   OPEN/HALF_OPEN transitions use wrlock (rare).
 *
 * @version 4.0.0
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
     * 0 = 4096. When full, new connections are rejected (closed immediately)
     * and EPOLLIN on the listen fd is disabled until queue drains below 70%.
     */
    uint32_t worker_queue_depth;

    /**
     * Milliseconds to wait for active connections to drain before force-stopping.
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
 */
distric_err_t tcp_server_create_with_config(
    const char*                bind_addr,
    uint16_t                   port,
    const tcp_server_config_t* cfg,
    metrics_registry_t*        metrics,
    logger_t*                  logger,
    tcp_server_t**             server
);

/**
 * @brief Start accepting connections.
 *
 * Spawns the accept thread and all worker threads.
 * Transitions state from STOPPED → RUNNING.
 *
 * @param server        Server handle.
 * @param on_connection Callback invoked for each new connection.
 * @param userdata      Passed through to @p on_connection.
 */
distric_err_t tcp_server_start(
    tcp_server_t*             server,
    tcp_connection_callback_t on_connection,
    void*                     userdata
);

/**
 * @brief Gracefully stop the server with deterministic draining.
 *
 * 1. Sets state to DRAINING — accept thread exits.
 * 2. Waits up to drain_timeout_ms for active_connections to reach 0.
 *    Waiting is condvar-based (no sleep polling).
 * 3. On timeout, calls force-stop: workers exit immediately, remaining
 *    queued connections are force-closed.
 * 4. Joins all worker threads.
 * 5. Nullifies observability pointers with seq_cst barrier.
 *
 * Safe to call from any thread. Idempotent.
 */
distric_err_t tcp_server_stop(tcp_server_t* server);

/**
 * @brief Stop and destroy the server.
 *
 * Calls tcp_server_stop() if needed, then frees all resources.
 */
void tcp_server_destroy(tcp_server_t* server);

/**
 * @brief Return the current server state.
 */
tcp_server_state_t tcp_server_get_state(const tcp_server_t* server);

/**
 * @brief Return the current count of active (in-flight) connections.
 */
int64_t tcp_server_active_connections(const tcp_server_t* server);

/* ============================================================================
 * TCP CLIENT API
 * ========================================================================= */

/**
 * @brief Connect to a remote host (non-blocking dial with timeout).
 *
 * @param host        Hostname or IP address.
 * @param port        Port number.
 * @param timeout_ms  Connection timeout in milliseconds (>0 required).
 * @param config      Connection configuration (NULL = defaults).
 * @param metrics     Metrics registry (may be NULL).
 * @param logger      Logger instance (may be NULL).
 * @param conn        [out] Created connection handle.
 *
 * @return DISTRIC_OK           on success.
 * @return DISTRIC_ERR_TIMEOUT  if connection did not complete in time.
 * @return DISTRIC_ERR_UNAVAILABLE if circuit breaker is OPEN.
 * @return DISTRIC_ERR_INIT_FAILED on OS error.
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

/* ============================================================================
 * TCP I/O API
 * ========================================================================= */

/**
 * @brief Send data over a connection.
 *
 * Non-blocking. Buffers into the per-connection ring-buffer send queue on
 * EAGAIN. Returns DISTRIC_ERR_BACKPRESSURE when queue >= HWM.
 *
 * @return > 0                     Bytes accepted (sent + queued).
 * @return DISTRIC_ERR_BACKPRESSURE Queue at HWM; caller must throttle.
 * @return DISTRIC_ERR_IO           Hard send error.
 */
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);

/**
 * @brief Receive data from a connection.
 *
 * Uses the cached per-connection epoll fd for readiness detection.
 *
 * @param conn        Connection handle.
 * @param buffer      Receive buffer.
 * @param len         Buffer size in bytes.
 * @param timeout_ms  -1 = non-blocking (preferred); 0 = infinite; >0 = wait.
 *
 * @deprecated timeout_ms >= 0 blocks inside this function and creates
 * tail-latency jitter under load. Migrate to:
 *   - timeout_ms = -1 (always non-blocking), plus
 *   - tcp_is_readable() for readiness polling from an external event loop.
 *
 * @return > 0             Bytes received.
 * @return 0               No data (timeout or WOULD_BLOCK).
 * @return DISTRIC_ERR_EOF Peer closed the connection cleanly.
 * @return DISTRIC_ERR_IO  Receive error.
 */
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);

/**
 * @brief Poll read readiness without blocking.
 *
 * Returns true if the connection has data available to read immediately.
 * Uses the cached per-connection epoll fd (zero-cost, no allocation).
 *
 * Preferred alternative to passing timeout_ms >= 0 to tcp_recv(). Callers
 * managing their own event loop should poll readiness here and call
 * tcp_recv() with timeout_ms = -1 only when this returns true.
 *
 * @param conn  Connection handle.
 * @return true if EPOLLIN is set; false otherwise (or if conn is NULL).
 */
bool tcp_is_readable(const tcp_connection_t* conn);

/**
 * @brief Return pending bytes in the send queue.
 */
size_t tcp_send_queue_depth(const tcp_connection_t* conn);

/**
 * @brief Return true if the send queue is below the HWM (safe to send more).
 */
bool tcp_is_writable(const tcp_connection_t* conn);

/**
 * @brief Return the remote address and port.
 */
distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char* addr_out, size_t addr_len,
    uint16_t* port_out
);

/**
 * @brief Return the unique monotonic connection ID.
 */
uint64_t tcp_get_connection_id(tcp_connection_t* conn);

/**
 * @brief Close and free the connection.
 *
 * After this call @p conn is invalid. Safe to call with NULL.
 */
void tcp_close(tcp_connection_t* conn);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_H */