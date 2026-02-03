/**
 * @file rpc.h
 * @brief RPC Framework for Request-Response Communication
 * 
 * Provides synchronous RPC over TCP with:
 * - Request-response pattern with unique request IDs
 * - Timeout and retry support
 * - Automatic metrics tracking (call count, duration, errors)
 * - Distributed tracing integration
 * - Connection pooling via distric_transport
 * - Thread-safe operations
 * 
 * Architecture:
 * - RPC Server: Registers handlers by message type, runs on TCP server
 * - RPC Client: Makes calls to remote servers with timeout/retry
 * - Request ID tracking for multiplexing multiple requests on one connection
 * 
 * Message Flow:
 * 1. Client serializes request with unique request_id
 * 2. Client sends message header + payload over TCP
 * 3. Server receives, dispatches to handler based on msg_type
 * 4. Server executes handler, generates response
 * 5. Server sends response header + payload back
 * 6. Client receives response, matches by request_id
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_PROTOCOL_RPC_H
#define DISTRIC_PROTOCOL_RPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_obs.h>
#include <distric_transport.h>
#include "binary.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * OPAQUE TYPES
 * ========================================================================= */

typedef struct rpc_server rpc_server_t;
typedef struct rpc_client rpc_client_t;

/* ============================================================================
 * RPC HANDLER SIGNATURE
 * ========================================================================= */

/**
 * @brief RPC request handler function
 * 
 * Called by RPC server when a message of registered type is received.
 * Handler must:
 * - Parse request payload
 * - Perform operation
 * - Serialize response payload
 * - Return 0 on success, negative error code on failure
 * 
 * Memory ownership:
 * - request: Read-only, valid for handler duration
 * - response: Handler must allocate (caller will free)
 * - span: Optional, for distributed tracing
 * 
 * @param request Request payload data
 * @param req_len Request payload length
 * @param response Output: Response payload (allocated by handler)
 * @param resp_len Output: Response payload length
 * @param userdata User-provided context data
 * @param span Trace span for this request (may be NULL)
 * @return 0 on success, negative error code on failure
 */
typedef int (*rpc_handler_t)(
    const uint8_t* request,
    size_t req_len,
    uint8_t** response,
    size_t* resp_len,
    void* userdata,
    trace_span_t* span
);

/* ============================================================================
 * RPC SERVER API
 * ========================================================================= */

/**
 * @brief Create RPC server
 * 
 * Creates an RPC server that listens on the provided TCP server.
 * Integrates with metrics, logging, and tracing.
 * 
 * @param tcp_server TCP server to use for accepting connections
 * @param metrics Metrics registry (required)
 * @param logger Logger instance (required)
 * @param tracer Tracer for distributed tracing (optional, may be NULL)
 * @param server_out Output: Created RPC server
 * @return DISTRIC_OK on success
 */
distric_err_t rpc_server_create(
    tcp_server_t* tcp_server,
    metrics_registry_t* metrics,
    logger_t* logger,
    tracer_t* tracer,
    rpc_server_t** server_out
);

/**
 * @brief Register RPC handler for a message type
 * 
 * Registers a handler function that will be called when messages
 * of the specified type are received.
 * 
 * @param server RPC server
 * @param msg_type Message type to handle
 * @param handler Handler function
 * @param userdata User data passed to handler
 * @return DISTRIC_OK on success
 */
distric_err_t rpc_server_register_handler(
    rpc_server_t* server,
    message_type_t msg_type,
    rpc_handler_t handler,
    void* userdata
);

/**
 * @brief Start RPC server
 * 
 * Starts accepting connections and processing RPC requests.
 * This is non-blocking and spawns background threads.
 * 
 * @param server RPC server
 * @return DISTRIC_OK on success
 */
distric_err_t rpc_server_start(rpc_server_t* server);

/**
 * @brief Stop RPC server
 * 
 * Stops accepting new connections and waits for in-flight
 * requests to complete.
 * 
 * @param server RPC server
 */
void rpc_server_stop(rpc_server_t* server);

/**
 * @brief Destroy RPC server
 * 
 * Stops server (if running) and frees all resources.
 * 
 * @param server RPC server
 */
void rpc_server_destroy(rpc_server_t* server);

/* ============================================================================
 * RPC CLIENT API
 * ========================================================================= */

/**
 * @brief Create RPC client
 * 
 * Creates an RPC client that uses the provided TCP connection pool
 * for making requests.
 * 
 * @param tcp_pool TCP connection pool (required)
 * @param metrics Metrics registry (required)
 * @param logger Logger instance (required)
 * @param tracer Tracer for distributed tracing (optional, may be NULL)
 * @param client_out Output: Created RPC client
 * @return DISTRIC_OK on success
 */
distric_err_t rpc_client_create(
    tcp_pool_t* tcp_pool,
    metrics_registry_t* metrics,
    logger_t* logger,
    tracer_t* tracer,
    rpc_client_t** client_out
);

/**
 * @brief Make synchronous RPC call
 * 
 * Sends request and waits for response with timeout.
 * 
 * Flow:
 * 1. Acquire TCP connection from pool
 * 2. Create trace span (if tracer available)
 * 3. Build message header with request payload
 * 4. Send header + payload
 * 5. Wait for response (with timeout)
 * 6. Validate response header and CRC
 * 7. Return response payload
 * 8. Release connection back to pool
 * 9. Update metrics
 * 
 * Memory ownership:
 * - request: Caller owns, read-only
 * - response: Allocated by this function, caller must free()
 * 
 * @param client RPC client
 * @param host Target host (IP or hostname)
 * @param port Target port
 * @param msg_type Message type for request
 * @param request Request payload
 * @param req_len Request payload length
 * @param response_out Output: Response payload (caller must free)
 * @param resp_len_out Output: Response payload length
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return DISTRIC_OK on success
 */
distric_err_t rpc_call(
    rpc_client_t* client,
    const char* host,
    uint16_t port,
    message_type_t msg_type,
    const uint8_t* request,
    size_t req_len,
    uint8_t** response_out,
    size_t* resp_len_out,
    int timeout_ms
);

/**
 * @brief Make RPC call with retry
 * 
 * Same as rpc_call() but retries on failure with exponential backoff.
 * 
 * @param client RPC client
 * @param host Target host
 * @param port Target port
 * @param msg_type Message type
 * @param request Request payload
 * @param req_len Request length
 * @param response_out Output: Response payload
 * @param resp_len_out Output: Response length
 * @param timeout_ms Timeout per attempt
 * @param max_retries Maximum retry attempts
 * @return DISTRIC_OK on success
 */
distric_err_t rpc_call_with_retry(
    rpc_client_t* client,
    const char* host,
    uint16_t port,
    message_type_t msg_type,
    const uint8_t* request,
    size_t req_len,
    uint8_t** response_out,
    size_t* resp_len_out,
    int timeout_ms,
    int max_retries
);

/**
 * @brief Destroy RPC client
 * 
 * Frees all resources. Does not destroy the TCP pool.
 * 
 * @param client RPC client
 */
void rpc_client_destroy(rpc_client_t* client);

/* ============================================================================
 * METRICS TRACKED
 * ========================================================================= */

/**
 * RPC Server Metrics:
 * - rpc_server_requests_total (counter, labels: msg_type)
 * - rpc_server_request_duration_seconds (histogram, labels: msg_type)
 * - rpc_server_errors_total (counter, labels: msg_type, error_type)
 * - rpc_server_active_requests (gauge)
 * 
 * RPC Client Metrics:
 * - rpc_client_calls_total (counter, labels: msg_type)
 * - rpc_client_call_duration_seconds (histogram, labels: msg_type)
 * - rpc_client_errors_total (counter, labels: msg_type, error_type)
 * - rpc_client_retries_total (counter, labels: msg_type)
 */

 /* RPC Server */
distric_err_t rpc_server_start(rpc_server_t* server);
distric_err_t rpc_server_stop(rpc_server_t* server);

/* These already exist per the implementation document */
distric_err_t rpc_server_create(tcp_server_t* tcp, metrics_registry_t* metrics, logger_t* logger, rpc_server_t** server_out);
void rpc_server_destroy(rpc_server_t* server);
distric_err_t rpc_register_handler(rpc_server_t* server, message_type_t msg_type, rpc_handler_t handler, void* userdata);
distric_err_t rpc_client_create(tcp_pool_t* pool, metrics_registry_t* metrics, logger_t* logger, rpc_client_t** client_out);
void rpc_client_destroy(rpc_client_t* client);
distric_err_t rpc_call(rpc_client_t* client, const char* host, uint16_t port, message_type_t msg_type, const uint8_t* request, size_t req_len, uint8_t** response, size_t* resp_len, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_RPC_H */