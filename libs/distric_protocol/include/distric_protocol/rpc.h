/**
 * @file rpc.h
 * @brief RPC Framework — Production-hardened request/response over TCP
 *
 * Applied improvements:
 *
 *  Improvement #7 — Handler unregistration with tombstone support.
 *    rpc_server_unregister_handler() is added to the public API.
 *    The hash table uses 0xFFFF as a tombstone sentinel so that open-addressing
 *    probe chains are not broken by removal.  Callers can now hot-reload
 *    handlers without restarting the server.
 *
 *  Improvement #9 — Total wall-clock timeout budget in retry loop.
 *    rpc_client_config_t gains a total_timeout_ms field (0 = unlimited,
 *    backward-compatible).  rpc_call_with_retry reads this budget and:
 *      a) trims the per-attempt timeout_ms so the last attempt cannot overshoot
 *         the wall-clock deadline;
 *      b) aborts early and returns DISTRIC_ERR_TIMEOUT when the budget is
 *         exhausted before max_retries is reached;
 *      c) increments rpc_client_budget_exceeded_total so operators can alert.
 *
 * Pre-existing improvements (retained):
 *  - P0: Maximum payload size enforcement (RPC_MAX_MESSAGE_SIZE)
 *  - P0: End-to-end backpressure propagation
 *  - P0: Per-RPC timeout with DISTRIC_ERR_TIMEOUT
 *  - Admission control: max_inflight_requests with atomic counter
 *  - Graceful drain: configurable drain_timeout_ms
 *  - Structured error taxonomy: rpc_error_class_t
 *  - Fix #4: rpc_client_config_t with pool_acquire_timeout_ms
 *  - Fix #5: O(1) handler dispatch table (hash table, open addressing)
 *
 * @version 4.0.0
 */

#ifndef DISTRIC_PROTOCOL_RPC_H
#define DISTRIC_PROTOCOL_RPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_transport.h>
#include <distric_obs.h>
#include "distric_protocol/binary.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PAYLOAD LIMITS
 * ========================================================================= */

/**
 * @brief Hard maximum for any incoming or outgoing message payload.
 *
 * Protects against malicious/malformed length fields triggering enormous
 * allocations.  Override at compile time via -DRPC_MAX_MESSAGE_SIZE.
 * Default: 16 MiB.  Acceptable range: 1 byte – 64 MiB.
 */
#ifndef RPC_MAX_MESSAGE_SIZE
#  define RPC_MAX_MESSAGE_SIZE (16u * 1024u * 1024u)
#endif

#if RPC_MAX_MESSAGE_SIZE == 0 || RPC_MAX_MESSAGE_SIZE > (64u * 1024u * 1024u)
#  error "RPC_MAX_MESSAGE_SIZE must be in [1, 67108864]"
#endif

/* ============================================================================
 * ADMISSION CONTROL DEFAULTS
 * ========================================================================= */

/** Default maximum concurrent in-flight server-side requests. */
#define RPC_DEFAULT_MAX_INFLIGHT             1024u

/** Default drain timeout (ms) before rpc_server_stop() returns forcefully. */
#define RPC_DEFAULT_DRAIN_TIMEOUT_MS         5000u

/**
 * @brief Default timeout (ms) for acquiring a connection from the pool.
 *
 * When all pool connections are in use, rpc_call() will wait up to this
 * duration before returning DISTRIC_ERR_TIMEOUT.
 */
#define RPC_DEFAULT_POOL_ACQUIRE_TIMEOUT_MS  5000u

/** @brief Default maximum concurrent outbound RPC calls. */
#define RPC_DEFAULT_MAX_CONCURRENT_CALLS     64u

/* ============================================================================
 * HANDLER PROTOTYPE
 * ========================================================================= */

/**
 * @brief Application-level message handler.
 *
 * @param request     Incoming payload (NOT owned; valid only for this call).
 * @param req_len     Payload length.
 * @param response    Output: heap-allocated response buffer.  The RPC framework
 *                    calls free() on this after sending.  Set *response = NULL
 *                    to send an empty response.
 * @param resp_len    Output: response length in bytes.
 * @param userdata    Opaque pointer supplied at handler registration.
 * @param span        Active trace span (may be NULL if tracing is disabled).
 * @return            0 on success; non-zero to suppress response transmission.
 */
typedef int (*rpc_handler_t)(
    const uint8_t* request,
    size_t         req_len,
    uint8_t**      response,
    size_t*        resp_len,
    void*          userdata,
    trace_span_t*  span
);

/* ============================================================================
 * OPAQUE TYPES
 * ========================================================================= */

typedef struct rpc_server rpc_server_t;
typedef struct rpc_client rpc_client_t;

/* ============================================================================
 * SERVER CONFIGURATION
 * ========================================================================= */

/**
 * @brief Optional server configuration.  Zero-initialise for all defaults.
 */
typedef struct {
    /**
     * Maximum number of concurrent in-flight requests.
     * Requests arriving when this limit is reached are rejected immediately.
     * 0 → use RPC_DEFAULT_MAX_INFLIGHT.
     */
    uint32_t max_inflight_requests;

    /**
     * Milliseconds to wait for in-flight requests to complete before
     * rpc_server_stop() returns.
     * 0 → use RPC_DEFAULT_DRAIN_TIMEOUT_MS.
     */
    uint32_t drain_timeout_ms;

    /**
     * Maximum accepted payload size in bytes.
     * Connections sending larger payloads are closed immediately.
     * 0 → use RPC_MAX_MESSAGE_SIZE.
     */
    uint32_t max_message_size;
} rpc_server_config_t;

/* ============================================================================
 * CLIENT CONFIGURATION
 * ========================================================================= */

/**
 * @brief Optional client configuration.  Zero-initialise for all defaults.
 */
typedef struct {
    /**
     * Maximum number of concurrent outbound calls (pool starvation guard).
     * rpc_call() blocks for at most pool_acquire_timeout_ms waiting for a
     * slot.  0 → use RPC_DEFAULT_MAX_CONCURRENT_CALLS.
     */
    uint32_t max_concurrent_calls;

    /**
     * Milliseconds to wait for a connection slot before returning
     * DISTRIC_ERR_TIMEOUT.  0 → use RPC_DEFAULT_POOL_ACQUIRE_TIMEOUT_MS.
     * Set to UINT32_MAX to block indefinitely (not recommended).
     */
    uint32_t pool_acquire_timeout_ms;

    /**
     * @brief Total wall-clock deadline for rpc_call_with_retry (ms).
     *
     * Improvement #9: bounds the total time rpc_call_with_retry() may spend
     * across ALL attempts plus inter-attempt backoff.  Per-attempt timeout_ms
     * is trimmed to the remaining budget on each iteration.
     *
     * 0 → no total budget (limited only by max_retries × timeout_ms).
     *
     * Recommended value: slightly less than the caller's own SLA timeout
     * (e.g. Raft election timeout, client session timeout).
     */
    uint32_t total_timeout_ms;
} rpc_client_config_t;

/* ============================================================================
 * ERROR TAXONOMY
 * ========================================================================= */

typedef enum {
    RPC_ERR_CLASS_OK,
    RPC_ERR_CLASS_TIMEOUT,
    RPC_ERR_CLASS_UNAVAILABLE,
    RPC_ERR_CLASS_OVERLOADED,
    RPC_ERR_CLASS_BACKPRESSURE,
    RPC_ERR_CLASS_INVALID,
    RPC_ERR_CLASS_INTERNAL,
} rpc_error_class_t;

/**
 * @brief Classify a distric_err_t into a retryability class.
 *
 * TIMEOUT, UNAVAILABLE, and BACKPRESSURE → retryable.
 * INVALID, INTERNAL → not retryable.
 */
rpc_error_class_t rpc_error_classify(distric_err_t err);

/** @brief Return a string name for an error class. */
const char* rpc_error_class_to_string(rpc_error_class_t cls);

/* ============================================================================
 * SERVER API
 * ========================================================================= */

/** @brief Create an RPC server with default configuration. */
distric_err_t rpc_server_create(
    tcp_server_t*       tcp_server,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tracer_t*           tracer,
    rpc_server_t**      server_out
);

/** @brief Create an RPC server with explicit configuration. */
distric_err_t rpc_server_create_with_config(
    tcp_server_t*              tcp_server,
    metrics_registry_t*        metrics,
    logger_t*                  logger,
    tracer_t*                  tracer,
    const rpc_server_config_t* config,
    rpc_server_t**             server_out
);

/**
 * @brief Register a handler for a message type.
 *
 * Thread-safe.  Handler is stored in an O(1) open-addressing hash table.
 * The dispatch lock is released BEFORE the application handler is invoked,
 * so slow handlers do NOT block concurrent registrations.
 *
 * Registering the same msg_type a second time replaces the previous handler.
 */
distric_err_t rpc_server_register_handler(
    rpc_server_t*  server,
    message_type_t msg_type,
    rpc_handler_t  handler,
    void*          userdata
);

/**
 * @brief Unregister a previously registered handler.
 *
 * Improvement #7: removes the handler for msg_type from the dispatch table.
 * Uses a tombstone sentinel (0xFFFF) so open-addressing probe chains remain
 * intact.  Thread-safe.
 *
 * @return DISTRIC_OK if the handler was found and removed.
 * @return DISTRIC_ERR_NOT_FOUND if no handler is registered for msg_type.
 */
distric_err_t rpc_server_unregister_handler(
    rpc_server_t*  server,
    message_type_t msg_type
);

/** @brief Start accepting connections (non-blocking; spawns worker threads). */
distric_err_t rpc_server_start(rpc_server_t* server);

/**
 * @brief Initiate graceful shutdown.
 *
 * 1. Sets accepting_requests = false — loops exit at next poll boundary.
 * 2. Waits up to drain_timeout_ms for in-flight handlers to finish.
 * 3. Calls tcp_server_stop() — no new connections accepted.
 */
void rpc_server_stop(rpc_server_t* server);

/** @brief Free all resources.  Calls stop() if not already stopped. */
void rpc_server_destroy(rpc_server_t* server);

/* ============================================================================
 * CLIENT API
 * ========================================================================= */

/** @brief Create an RPC client with default configuration. */
distric_err_t rpc_client_create(
    tcp_pool_t*         tcp_pool,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tracer_t*           tracer,
    rpc_client_t**      client_out
);

/** @brief Create an RPC client with explicit configuration. */
distric_err_t rpc_client_create_with_config(
    tcp_pool_t*               tcp_pool,
    metrics_registry_t*       metrics,
    logger_t*                 logger,
    tracer_t*                 tracer,
    const rpc_client_config_t* config,
    rpc_client_t**            client_out
);

/**
 * @brief Make a synchronous RPC call.
 *
 * Error mapping:
 *  DISTRIC_ERR_TIMEOUT        → recv timeout or pool-acquire timeout exceeded
 *  DISTRIC_ERR_BACKPRESSURE   → send queue full (retryable with backoff)
 *  DISTRIC_ERR_UNAVAILABLE    → connection failure (retryable)
 *  DISTRIC_ERR_INVALID_ARG    → bad arguments (not retryable)
 *  DISTRIC_ERR_INVALID_FORMAT → bad response header or CRC (not retryable)
 *  DISTRIC_ERR_NO_MEMORY      → allocation failure (not retryable)
 *
 * Memory: *response_out is heap-allocated; caller must free() it.
 *
 * @param timeout_ms  Per-call deadline in milliseconds.  0 = no recv timeout.
 */
distric_err_t rpc_call(
    rpc_client_t*  client,
    const char*    host,
    uint16_t       port,
    message_type_t msg_type,
    const uint8_t* request,
    size_t         req_len,
    uint8_t**      response_out,
    size_t*        resp_len_out,
    int            timeout_ms
);

/**
 * @brief Make an RPC call with automatic exponential-backoff retries.
 *
 * Retries only on retryable error classes (TIMEOUT, UNAVAILABLE, BACKPRESSURE).
 * Backoff includes ±25 % random jitter to prevent thundering-herd behaviour.
 *
 * Improvement #9: honours total_timeout_ms from the client's config.  If set,
 * the per-attempt timeout_ms is trimmed to the remaining wall-clock budget so
 * the overall call never exceeds total_timeout_ms.  The metric
 * rpc_client_budget_exceeded_total is incremented when the budget expires
 * before max_retries is reached.
 *
 * @param timeout_ms  Per-attempt timeout in milliseconds.
 * @param max_retries Maximum number of retry attempts after the first failure.
 */
distric_err_t rpc_call_with_retry(
    rpc_client_t*  client,
    const char*    host,
    uint16_t       port,
    message_type_t msg_type,
    const uint8_t* request,
    size_t         req_len,
    uint8_t**      response_out,
    size_t*        resp_len_out,
    int            timeout_ms,
    int            max_retries
);

/** @brief Free RPC client resources.  Does NOT destroy the TCP pool. */
void rpc_client_destroy(rpc_client_t* client);

/* ============================================================================
 * METRICS TRACKED
 *
 * Server:
 *   rpc_server_requests_total            counter
 *   rpc_server_request_duration_ms       histogram
 *   rpc_server_errors_total              counter
 *   rpc_server_active_requests           gauge     ← updated on every change
 *   rpc_server_rejected_overload_total   counter
 *   rpc_server_rejected_payload_total    counter
 *
 * Client:
 *   rpc_client_calls_total               counter
 *   rpc_client_call_duration_ms          histogram
 *   rpc_client_errors_total              counter
 *   rpc_client_retries_total             counter
 *   rpc_client_timeout_total             counter
 *   rpc_client_backpressure_total        counter
 *   rpc_client_pool_timeout_total        counter
 *   rpc_client_budget_exceeded_total     counter   ← improvement #9
 * ========================================================================= */

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_RPC_H */