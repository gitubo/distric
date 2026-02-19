/**
 * @file tcp.h
 * @brief DistriC TCP Transport — Non-Blocking API v5
 *
 * All sockets are strictly non-blocking (O_NONBLOCK). The library never
 * sleeps inside a send or receive call.
 *
 * ==========================================================================
 * CONCURRENCY MODEL (v4+)
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
 * WORKER POOL SATURATION PROTECTION (v5)
 * ==========================================================================
 *
 *   Worker utilisation is tracked via an atomic busy_workers counter.
 *   The ratio busy_workers / worker_count is exported as a Prometheus gauge:
 *     tcp_worker_busy_ratio
 *
 *   Each handler invocation is timed with CLOCK_MONOTONIC. If the duration
 *   exceeds handler_warn_duration_ms (in tcp_server_config_t), a LOG_WARN
 *   is emitted and tcp_handler_slow_total is incremented. This makes slow
 *   handlers immediately visible without requiring external profiling.
 *
 *   Additionally, queue depth is exported as:
 *     tcp_worker_queue_depth
 *
 * ==========================================================================
 * ACCEPT BACKPRESSURE (v4+)
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
 * GLOBAL MEMORY BUDGET (v5)
 * ==========================================================================
 *
 *   New connections are rejected (DISTRIC_ERR_ALLOC_FAILURE) when the
 *   process-wide transport send-queue allocation would exceed
 *   transport_global_config_t.max_transport_memory_bytes. This prevents
 *   OOM crashes under downstream slowdowns.
 *
 *   Metric: connections_rejected_memory_total (in transport_config module).
 *
 * ==========================================================================
 * GRACEFUL SHUTDOWN (v4+)
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
 * I/O MODEL (v4+)
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
 * OBSERVABILITY LIFECYCLE (v4+)
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
 * ==========================================================================
 * API VERSIONING AND COMPATIBILITY (v5)
 * ==========================================================================
 *
 *   Semantic versioning applies to the public API. DISTRIC_DEPRECATED marks
 *   functions/parameters scheduled for removal in the next major release.
 *   Deprecated items will be removed in v6.0.0. See migration notes below.
 *
 *   Migration notes:
 *   - tcp_recv(timeout_ms > 0): use tcp_is_readable() + tcp_recv(-1) instead.
 *   - tcp_server_create() without config: prefer tcp_server_create_with_config()
 *     to take advantage of handler_warn_duration_ms and other v5 config fields.
 *
 * @version 5.0.0
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
 * DEPRECATION MACRO (Item 7)
 *
 * Usage:
 *   DISTRIC_DEPRECATED("use foo_v2() instead")
 *   void old_function(void);
 * ========================================================================= */

#if defined(__GNUC__) || defined(__clang__)
#  define DISTRIC_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#  define DISTRIC_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#  define DISTRIC_DEPRECATED(msg) /* no-op */
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

    /**
     * @brief (v5) Slow-handler warning threshold in milliseconds.
     *
     * If a connection callback runs longer than this value, a LOG_WARN is
     * emitted and tcp_handler_slow_total is incremented.
     * 0 = disabled (also falls back to transport_global_config_t.handler_warn_duration_ms).
     *
     * Recommended: 500–2000 ms for most services. Set to 0 to use the global
     * default configured via transport_config_apply().
     */
    uint32_t handler_warn_duration_ms;

    /**
     * @brief (v5) Per-server observability sampling percentage (1–100).
     *
     * Overrides transport_global_config_t.observability_sample_pct for this
     * server's accept and dispatch log calls. 0 = inherit global setting.
     * Does not affect WARN or ERROR — those are always emitted.
     */
    uint32_t observability_sample_pct;
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
 * @deprecated Prefer tcp_server_create_with_config() to access v5 features
 *             (handler_warn_duration_ms, observability_sample_pct).
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
    tcp_server_t**             server_out
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

/**
 * @brief (v5) Return the current worker busy ratio as a float in [0.0, 1.0].
 *
 * Ratio = busy_workers / worker_count.
 * Thread-safe, lock-free atomic read.
 * Useful for health checks and adaptive load-shedding decisions.
 */
float tcp_server_worker_busy_ratio(const tcp_server_t* server);

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
 * @return DISTRIC_OK             on success.
 * @return DISTRIC_ERR_TIMEOUT    if connection did not complete in time.
 * @return DISTRIC_ERR_UNAVAILABLE if circuit breaker is OPEN.
 * @return DISTRIC_ERR_INIT_FAILED on OS error.
 * @return DISTRIC_ERR_ALLOC_FAILURE if global memory budget is exhausted.
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
 * @brief Non-blocking send with automatic buffering.
 *
 * Attempts a direct kernel send. On EAGAIN, data is appended to the
 * per-connection send queue. Returns DISTRIC_ERR_BACKPRESSURE when the
 * queue exceeds its high-water mark.
 *
 * @param conn  Connection handle.
 * @param data  Data to send.
 * @param len   Number of bytes to send.
 * @return > 0                  Number of bytes sent or queued.
 * @return DISTRIC_ERR_BACKPRESSURE Queue HWM exceeded; stop sending.
 * @return DISTRIC_ERR_IO          Socket error (connection broken).
 */
int tcp_send(tcp_connection_t* conn, const void* data, size_t len);

/**
 * @brief Attempt to flush the send queue to the kernel.
 *
 * Call after a backpressure condition clears (e.g., after the peer
 * acknowledges data and EPOLLOUT fires). Returns DISTRIC_ERR_IO on error.
 *
 * @param conn Connection handle.
 * @return DISTRIC_OK or DISTRIC_ERR_IO.
 */
int tcp_flush(tcp_connection_t* conn);

/**
 * @brief Non-blocking receive.
 *
 * @param conn       Connection handle.
 * @param buffer     Receive buffer.
 * @param len        Buffer size.
 * @param timeout_ms Receive timeout:
 *                    -1 = non-blocking (returns 0 if no data).
 *                     0 = block indefinitely.
 *                    >0 = block up to timeout_ms ms.
 *
 * @note timeout_ms > 0 is DEPRECATED. Prefer timeout_ms = -1 combined with
 *       tcp_is_readable() or an external epoll loop.
 *
 * @return > 0  Bytes received.
 * @return   0  No data available (non-blocking / timeout).
 * @return < 0  DISTRIC_ERR_IO (connection closed or error).
 */
int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms);

/**
 * @brief Poll connection readability without blocking.
 *
 * Uses a cached per-connection epoll fd. O(1), no syscall on each call
 * (epoll_wait with timeout 0).
 *
 * @return true  Data available for reading.
 * @return false No data or connection error.
 */
bool tcp_is_readable(tcp_connection_t* conn);

/**
 * @brief Poll whether the connection send path is unblocked.
 *
 * Returns true when the per-connection send queue is below its high-water
 * mark, meaning tcp_send() will accept data without returning
 * DISTRIC_ERR_BACKPRESSURE.
 *
 * Thread-safe (acquires send_lock briefly).
 *
 * @return true  Send queue below HWM — safe to write.
 * @return false Send queue at or above HWM — backpressure active.
 */
bool tcp_is_writable(tcp_connection_t* conn);

/**
 * @brief Return the number of bytes currently buffered in the send queue.
 *
 * Useful for monitoring and adaptive flow control. Thread-safe.
 *
 * @return Bytes pending in the send queue, or 0 for NULL conn.
 */
size_t tcp_send_queue_depth(tcp_connection_t* conn);

/**
 * @brief Close and free a TCP connection.
 *
 * Releases the send queue (updating the global memory counter) and closes
 * the file descriptor. Safe to call from any thread that owns the connection.
 */
void tcp_close(tcp_connection_t* conn);

/**
 * @brief Return the raw file descriptor of a connection.
 *
 * Intended for use in external epoll loops. Do NOT call close() on the fd
 * directly — use tcp_close() instead.
 */
int tcp_connection_get_fd(const tcp_connection_t* conn);

/**
 * @brief Fill @p addr_out and @p port_out with the remote address and port.
 *
 * @param conn      Connection handle.
 * @param addr_out  Buffer to receive the dotted-decimal IP string (min 64 bytes).
 * @param addr_len  Size of @p addr_out.
 * @param port_out  [out] Remote port number.
 * @return DISTRIC_OK or DISTRIC_ERR_INVALID_ARG.
 */
distric_err_t tcp_get_remote_addr(
    const tcp_connection_t* conn,
    char*                   addr_out,
    size_t                  addr_len,
    uint16_t*               port_out
);

/**
 * @brief Return the unique monotonically-increasing connection identifier.
 *
 * Assigned at connection_alloc() time from a global atomic counter.
 * Zero is never a valid id.
 */
uint64_t tcp_get_connection_id(const tcp_connection_t* conn);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_TCP_H */