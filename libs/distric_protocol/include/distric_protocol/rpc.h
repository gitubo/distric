/**
 * @file rpc.h
 * @brief RPC Framework - Production-hardened request/response over TCP
 *
 * Fixes applied in this version:
 *  - P0: Maximum payload size enforcement (RPC_MAX_MESSAGE_SIZE)
 *  - P0: End-to-end backpressure propagation (send queue awareness)
 *  - P0: Per-RPC timeout with DISTRIC_ERR_TIMEOUT
 *  - Admission control: max_inflight_requests with atomic counter
 *  - Graceful drain: configurable drain_timeout_ms
 *  - Structured error taxonomy: rpc_error_class_t
 *  - Fix #4: rpc_client_config_t with pool_acquire_timeout_ms
 *            rpc_client_create_with_config() — pool starvation now returns
 *            DISTRIC_ERR_TIMEOUT instead of blocking indefinitely
 *  - Fix #5: O(1) handler dispatch table (hash table, open addressing)
 *            Handler lock released BEFORE calling application handler
 *
 * @version 3.0.0
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
#define RPC_MAX_MESSAGE_SIZE (16u * 1024u * 1024u)
#endif

#if RPC_MAX_MESSAGE_SIZE == 0 || RPC_MAX_MESSAGE_SIZE > (64u * 1024u * 1024u)
#error "RPC_MAX_MESSAGE_SIZE must be in [1, 67108864]"
#endif

/* ============================================================================
 * ADMISSION CONTROL DEFAULTS
 * ========================================================================= */

/** Default maximum concurrent in-flight server-side requests. */
#define RPC_DEFAULT_MAX_INFLIGHT      1024

/** Default drain timeout (ms) before rpc_server_stop() returns forcefully. */
#define RPC_DEFAULT_DRAIN_TIMEOUT_MS  5000

/**
 * @brief Default timeout (ms) for acquiring a connection from the pool.
 *
 * When all pool connections are in use, rpc_call() will wait up to this
 * duration before returning DISTRIC_ERR_TIMEOUT.  0 = block indefinitely
 * (strongly discouraged in production).
 */
#define RPC_DEFAULT_POOL_ACQUIRE_TIMEOUT_MS 5000

/**
 * @brief Default maximum concurrent outbound RPC calls.
 *
 * Acts as a semaphore cap to prevent connection pool exhaustion.
 * Should be set to the TCP pool size (or smaller).
 */
#define RPC_DEFAULT_MAX_CONCURRENT_CALLS 64

/* ============================================================================
 * STRUCTURED ERROR TAXONOMY
 * ========================================================================= */

/**
 * @brief Stable error classes for RPC-layer failures.
 *
 * Maps transport and protocol errors into actionable categories so callers
 * can decide retry policy without inspecting raw distric_err_t codes.
 */
typedef enum {
    RPC_ERR_CLASS_OK          = 0, /**< No error                                */
    RPC_ERR_CLASS_TIMEOUT     = 1, /**< Deadline exceeded — retryable           */
    RPC_ERR_CLASS_UNAVAILABLE = 2, /**< Connection failure — retryable          */
    RPC_ERR_CLASS_OVERLOADED  = 3, /**< Server overloaded — retryable w/ backoff */
    RPC_ERR_CLASS_BACKPRESSURE= 4, /**< Send queue full — retryable w/ backoff  */
    RPC_ERR_CLASS_INVALID     = 5, /**< Bad request / protocol violation — NOT retryable */
    RPC_ERR_CLASS_INTERNAL    = 6, /**< Internal server error — NOT retryable   */
} rpc_error_class_t;

/** @brief Return the string name of an error class. Never NULL. */
const char* rpc_error_class_to_string(rpc_error_class_t cls);

/** @brief Map a distric_err_t code to an RPC error class. */
rpc_error_class_t rpc_error_classify(distric_err_t err);

/* ============================================================================
 * RPC HANDLER TYPE
 * ========================================================================= */

/**
 * @brief RPC request handler callback.
 *
 * @param request     Incoming payload (NOT owned; valid only for this call).
 * @param req_len     Payload length.
 * @param response    Output: heap-allocated response buffer.  RPC framework
 *                    free()s this after sending.  Set to NULL to send empty.
 * @param resp_len    Output: response length.
 * @param userdata    Opaque pointer supplied at handler registration.
 * @param span        Active trace span (may be NULL).
 * @return            0 on success; negative value on application error.
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
     * Requests received when this limit is reached are rejected with
     * DISTRIC_ERR_UNAVAILABLE; the rpc_server_rejected_overload_total metric
     * is incremented.
     * 0 → use RPC_DEFAULT_MAX_INFLIGHT.
     */
    uint32_t max_inflight_requests;

    /**
     * Milliseconds to wait for in-flight requests to drain before
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
 * CLIENT CONFIGURATION — Fix #4
 * ========================================================================= */

/**
 * @brief Optional client configuration.  Zero-initialise for all defaults.
 *
 * Fix #4 — Pool starvation prevention:
 *   pool_acquire_timeout_ms bounds the time rpc_call() will wait for a free
 *   connection.  Without this, all callers block indefinitely when the pool
 *   is exhausted, causing cascading latency across the whole node.
 *
 *   max_concurrent_calls is implemented as a counting semaphore.  It should
 *   be ≤ the TCP pool size.  When all slots are taken and a new call arrives,
 *   sem_timedwait() fires after pool_acquire_timeout_ms and returns
 *   DISTRIC_ERR_TIMEOUT to the caller.
 */
typedef struct {
    /**
     * Maximum number of concurrent outbound calls.
     * rpc_call() blocks for at most pool_acquire_timeout_ms waiting for a
     * slot.  0 → use RPC_DEFAULT_MAX_CONCURRENT_CALLS.
     */
    uint32_t max_concurrent_calls;

    /**
     * Milliseconds to wait for a connection slot before returning
     * DISTRIC_ERR_TIMEOUT.  0 → use RPC_DEFAULT_POOL_ACQUIRE_TIMEOUT_MS.
     * Set to UINT32_MAX to disable the timeout (block indefinitely —
     * not recommended in production).
     */
    uint32_t pool_acquire_timeout_ms;
} rpc_client_config_t;

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
 * Thread-safe.  Handler is stored in an O(1) hash table (Fix #5).
 * The application handler is called with the dispatch lock already released,
 * so slow handlers do NOT block concurrent registrations.
 */
distric_err_t rpc_server_register_handler(
    rpc_server_t*  server,
    message_type_t msg_type,
    rpc_handler_t  handler,
    void*          userdata
);

/** @brief Start accepting connections (non-blocking; spawns worker threads). */
distric_err_t rpc_server_start(rpc_server_t* server);

/**
 * @brief Initiate graceful shutdown.
 *
 * 1. Sets accepting_requests = false — loops exit at next poll boundary.
 * 2. Calls tcp_server_stop() — no new connections accepted.
 * 3. Waits up to drain_timeout_ms for in-flight handlers to finish.
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

/**
 * @brief Create an RPC client with explicit configuration (Fix #4).
 *
 * Use this overload to configure pool-acquisition timeout and concurrency cap.
 *
 * @param config  Optional client tuning.  NULL → all defaults.
 */
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
 *  - DISTRIC_ERR_TIMEOUT      → recv timeout or pool acquire timeout exceeded
 *  - DISTRIC_ERR_BACKPRESSURE → send queue full (retryable with backoff)
 *  - DISTRIC_ERR_UNAVAILABLE  → connection failure (retryable)
 *  - DISTRIC_ERR_INVALID_ARG  → bad arguments (not retryable)
 *  - DISTRIC_ERR_INVALID_FORMAT → bad response header/CRC (not retryable)
 *  - DISTRIC_ERR_NO_MEMORY    → allocation failure (not retryable)
 *
 * Memory: *response_out is heap-allocated; caller must free().
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
 * ========================================================================= */

/**
 * Server metrics:
 *   rpc_server_requests_total          counter
 *   rpc_server_request_duration_ms     histogram
 *   rpc_server_errors_total            counter
 *   rpc_server_active_requests         gauge
 *   rpc_server_rejected_overload_total counter
 *   rpc_server_rejected_payload_total  counter
 *
 * Client metrics:
 *   rpc_client_calls_total             counter
 *   rpc_client_call_duration_ms        histogram
 *   rpc_client_errors_total            counter
 *   rpc_client_retries_total           counter
 *   rpc_client_timeout_total           counter
 *   rpc_client_backpressure_total      counter
 *   rpc_client_pool_timeout_total      counter   (Fix #4: pool acquire timeout)
 */

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_RPC_H */