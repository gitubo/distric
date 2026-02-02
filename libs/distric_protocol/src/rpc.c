/**
 * @file rpc.c
 * @brief RPC Framework Implementation
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/rpc.h"
#include "distric_protocol/crc32.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>

/* ============================================================================
 * CONSTANTS
 * ========================================================================= */

#define MAX_HANDLERS 64
#define MAX_PENDING_REQUESTS 1024
#define DEFAULT_RETRY_BACKOFF_MS 100

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

/** Handler registration */
typedef struct {
    message_type_t msg_type;
    rpc_handler_t handler;
    void* userdata;
} handler_entry_t;

/** RPC Server */
struct rpc_server {
    tcp_server_t* tcp_server;
    metrics_registry_t* metrics;
    logger_t* logger;
    tracer_t* tracer;
    
    handler_entry_t handlers[MAX_HANDLERS];
    size_t handler_count;
    pthread_rwlock_t handlers_lock;
    
    _Atomic bool running;
    
    /* Metrics */
    metric_t* requests_total;
    metric_t* request_duration;
    metric_t* errors_total;
    metric_t* active_requests;
};

/** RPC Client */
struct rpc_client {
    tcp_pool_t* tcp_pool;
    metrics_registry_t* metrics;
    logger_t* logger;
    tracer_t* tracer;
    
    _Atomic uint64_t request_id_counter;
    
    /* Metrics */
    metric_t* calls_total;
    metric_t* call_duration;
    metric_t* errors_total;
    metric_t* retries_total;
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

static uint64_t get_timestamp_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint64_t get_timestamp_ms(void) {
    return get_timestamp_us() / 1000;
}

/* ============================================================================
 * RPC SERVER IMPLEMENTATION
 * ========================================================================= */

/** Connection handler for RPC server */
static void rpc_server_handle_connection(tcp_connection_t* conn, void* userdata) {
    rpc_server_t* server = (rpc_server_t*)userdata;
    
    while (atomic_load(&server->running)) {
        /* Read message header */
        uint8_t header_buf[MESSAGE_HEADER_SIZE];
        int received = tcp_recv(conn, header_buf, MESSAGE_HEADER_SIZE, 1000);
        
        if (received == 0) {
            /* Connection closed */
            break;
        }
        
        if (received < 0) {
            if (errno == ETIMEDOUT || errno == EAGAIN) {
                continue;  /* Timeout, try again */
            }
            LOG_ERROR(server->logger, "rpc_server", "Failed to receive header",
                     "error", strerror(errno));
            break;
        }
        
        if (received != MESSAGE_HEADER_SIZE) {
            LOG_ERROR(server->logger, "rpc_server", "Partial header received",
                     "received", (char*)&received);
            break;
        }
        
        /* Deserialize header */
        message_header_t header;
        if (deserialize_header(header_buf, &header) != DISTRIC_OK) {
            LOG_ERROR(server->logger, "rpc_server", "Failed to deserialize header");
            break;
        }
        
        /* Validate header */
        if (!validate_message_header(&header)) {
            LOG_ERROR(server->logger, "rpc_server", "Invalid message header",
                     "magic", (char*)&header.magic);
            break;
        }
        
        /* Read payload */
        uint8_t* payload = NULL;
        if (header.payload_len > 0) {
            payload = (uint8_t*)malloc(header.payload_len);
            if (!payload) {
                LOG_ERROR(server->logger, "rpc_server", "Failed to allocate payload buffer");
                break;
            }
            
            received = tcp_recv(conn, payload, header.payload_len, 5000);
            if (received != (int)header.payload_len) {
                LOG_ERROR(server->logger, "rpc_server", "Failed to receive payload");
                free(payload);
                break;
            }
        }
        
        /* Verify CRC32 */
        if (!verify_message_crc32(&header, payload, header.payload_len)) {
            LOG_ERROR(server->logger, "rpc_server", "CRC32 verification failed");
            free(payload);
            metrics_counter_inc(server->errors_total);
            break;
        }
        
        /* Find handler */
        pthread_rwlock_rdlock(&server->handlers_lock);
        
        handler_entry_t* handler_entry = NULL;
        for (size_t i = 0; i < server->handler_count; i++) {
            if (server->handlers[i].msg_type == header.msg_type) {
                handler_entry = &server->handlers[i];
                break;
            }
        }
        
        if (!handler_entry) {
            pthread_rwlock_unlock(&server->handlers_lock);
            LOG_WARN(server->logger, "rpc_server", "No handler registered",
                    "msg_type", message_type_to_string(header.msg_type));
            free(payload);
            metrics_counter_inc(server->errors_total);
            continue;
        }
        
        /* Start trace span if tracer available */
        trace_span_t* span = NULL;
        if (server->tracer) {
            trace_start_span(server->tracer, "rpc_handle_request", &span);
            trace_add_tag(span, "msg_type", message_type_to_string(header.msg_type));
        }
        
        /* Update metrics */
        metrics_gauge_set(server->active_requests, 
                         metrics_gauge_set(server->active_requests, 0) + 1);
        
        uint64_t start_time = get_timestamp_us();
        
        /* Execute handler */
        uint8_t* response = NULL;
        size_t resp_len = 0;
        
        int result = handler_entry->handler(
            payload,
            header.payload_len,
            &response,
            &resp_len,
            handler_entry->userdata,
            span
        );
        
        pthread_rwlock_unlock(&server->handlers_lock);
        
        /* Calculate duration */
        uint64_t duration_us = get_timestamp_us() - start_time;
        metrics_histogram_observe(server->request_duration, duration_us / 1000000.0);
        metrics_counter_inc(server->requests_total);
        metrics_gauge_set(server->active_requests,
                         metrics_gauge_set(server->active_requests, 0) - 1);
        
        /* Build response header */
        message_header_t resp_header;
        message_header_init(&resp_header, header.msg_type, resp_len);
        resp_header.flags = MSG_FLAG_RESPONSE;
        resp_header.message_id = header.message_id;  /* Echo request ID */
        
        /* Compute CRC */
        compute_header_crc32(&resp_header, response, resp_len);
        
        /* Serialize response header */
        uint8_t resp_header_buf[MESSAGE_HEADER_SIZE];
        serialize_header(&resp_header, resp_header_buf);
        
        /* Send response */
        tcp_send(conn, resp_header_buf, MESSAGE_HEADER_SIZE);
        if (response && resp_len > 0) {
            tcp_send(conn, response, resp_len);
        }
        
        /* Cleanup */
        free(payload);
        free(response);
        
        /* Finish trace span */
        if (span) {
            if (result == 0) {
                trace_set_status(span, SPAN_STATUS_OK);
            } else {
                trace_set_status(span, SPAN_STATUS_ERROR);
            }
            trace_finish_span(server->tracer, span);
        }
        
        LOG_DEBUG(server->logger, "rpc_server", "Request handled",
                 "msg_type", message_type_to_string(header.msg_type),
                 "duration_ms", (char*)&duration_us);
    }
    
    tcp_close(conn);
}

distric_err_t rpc_server_create(
    tcp_server_t* tcp_server,
    metrics_registry_t* metrics,
    logger_t* logger,
    tracer_t* tracer,
    rpc_server_t** server_out
) {
    if (!tcp_server || !metrics || !logger || !server_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    rpc_server_t* server = (rpc_server_t*)calloc(1, sizeof(rpc_server_t));
    if (!server) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    server->tcp_server = tcp_server;
    server->metrics = metrics;
    server->logger = logger;
    server->tracer = tracer;
    server->handler_count = 0;
    atomic_store(&server->running, false);
    
    pthread_rwlock_init(&server->handlers_lock, NULL);
    
    /* Register metrics */
    metrics_register_counter(metrics, "rpc_server_requests_total",
                            "Total RPC requests", NULL, 0, &server->requests_total);
    
    metrics_register_histogram(metrics, "rpc_server_request_duration_seconds",
                              "RPC request duration", NULL, 0, &server->request_duration);
    
    metrics_register_counter(metrics, "rpc_server_errors_total",
                            "Total RPC errors", NULL, 0, &server->errors_total);
    
    metrics_register_gauge(metrics, "rpc_server_active_requests",
                          "Active RPC requests", NULL, 0, &server->active_requests);
    
    *server_out = server;
    return DISTRIC_OK;
}

distric_err_t rpc_server_register_handler(
    rpc_server_t* server,
    message_type_t msg_type,
    rpc_handler_t handler,
    void* userdata
) {
    if (!server || !handler) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&server->handlers_lock);
    
    if (server->handler_count >= MAX_HANDLERS) {
        pthread_rwlock_unlock(&server->handlers_lock);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    /* Check for duplicate */
    for (size_t i = 0; i < server->handler_count; i++) {
        if (server->handlers[i].msg_type == msg_type) {
            pthread_rwlock_unlock(&server->handlers_lock);
            return DISTRIC_ERR_INVALID_ARG;  /* Already registered */
        }
    }
    
    server->handlers[server->handler_count].msg_type = msg_type;
    server->handlers[server->handler_count].handler = handler;
    server->handlers[server->handler_count].userdata = userdata;
    server->handler_count++;
    
    pthread_rwlock_unlock(&server->handlers_lock);
    
    LOG_INFO(server->logger, "rpc_server", "Handler registered",
            "msg_type", message_type_to_string(msg_type));
    
    return DISTRIC_OK;
}

distric_err_t rpc_server_start(rpc_server_t* server) {
    if (!server) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    atomic_store(&server->running, true);
    
    /* Start TCP server with our connection handler */
    int result = tcp_server_start(server->tcp_server, 
                                  rpc_server_handle_connection,
                                  server);
    
    if (result != 0) {
        atomic_store(&server->running, false);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    LOG_INFO(server->logger, "rpc_server", "RPC server started");
    
    return DISTRIC_OK;
}

void rpc_server_stop(rpc_server_t* server) {
    if (!server) {
        return;
    }
    
    atomic_store(&server->running, false);
    
    LOG_INFO(server->logger, "rpc_server", "RPC server stopping");
    
    /* Wait for active requests to complete (with timeout) */
    for (int i = 0; i < 100; i++) {
        double active = 0;
        metrics_gauge_set(server->active_requests, active);  /* Get current value */
        if (active == 0) {
            break;
        }
        usleep(100000);  /* 100ms */
    }
    
    LOG_INFO(server->logger, "rpc_server", "RPC server stopped");
}

void rpc_server_destroy(rpc_server_t* server) {
    if (!server) {
        return;
    }
    
    rpc_server_stop(server);
    
    pthread_rwlock_destroy(&server->handlers_lock);
    free(server);
}

/* ============================================================================
 * RPC CLIENT IMPLEMENTATION
 * ========================================================================= */

distric_err_t rpc_client_create(
    tcp_pool_t* tcp_pool,
    metrics_registry_t* metrics,
    logger_t* logger,
    tracer_t* tracer,
    rpc_client_t** client_out
) {
    if (!tcp_pool || !metrics || !logger || !client_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    rpc_client_t* client = (rpc_client_t*)calloc(1, sizeof(rpc_client_t));
    if (!client) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    client->tcp_pool = tcp_pool;
    client->metrics = metrics;
    client->logger = logger;
    client->tracer = tracer;
    atomic_store(&client->request_id_counter, 1);
    
    /* Register metrics */
    metrics_register_counter(metrics, "rpc_client_calls_total",
                            "Total RPC calls", NULL, 0, &client->calls_total);
    
    metrics_register_histogram(metrics, "rpc_client_call_duration_seconds",
                              "RPC call duration", NULL, 0, &client->call_duration);
    
    metrics_register_counter(metrics, "rpc_client_errors_total",
                            "Total RPC errors", NULL, 0, &client->errors_total);
    
    metrics_register_counter(metrics, "rpc_client_retries_total",
                            "Total RPC retries", NULL, 0, &client->retries_total);
    
    *client_out = client;
    return DISTRIC_OK;
}

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
) {
    if (!client || !host || !response_out || !resp_len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Start trace span */
    trace_span_t* span = NULL;
    if (client->tracer) {
        trace_start_span(client->tracer, "rpc_call", &span);
        trace_add_tag(span, "msg_type", message_type_to_string(msg_type));
        trace_add_tag(span, "host", host);
        
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        trace_add_tag(span, "port", port_str);
    }
    
    uint64_t start_time = get_timestamp_us();
    distric_err_t result = DISTRIC_OK;
    
    /* Acquire connection from pool */
    tcp_connection_t* conn = NULL;
    int acquire_result = tcp_pool_acquire(client->tcp_pool, host, port, &conn);
    
    if (acquire_result != 0 || !conn) {
        LOG_ERROR(client->logger, "rpc_client", "Failed to acquire connection",
                 "host", host);
        result = DISTRIC_ERR_INIT_FAILED;
        goto cleanup;
    }
    
    /* Build request header */
    message_header_t req_header;
    message_header_init(&req_header, msg_type, req_len);
    req_header.message_id = atomic_fetch_add(&client->request_id_counter, 1);
    
    /* Inject trace context if available */
    if (span) {
        /* Encode trace context in flags (simplified) */
        req_header.flags |= MSG_FLAG_URGENT;  /* Use as trace flag */
    }
    
    /* Compute CRC */
    compute_header_crc32(&req_header, request, req_len);
    
    /* Serialize header */
    uint8_t header_buf[MESSAGE_HEADER_SIZE];
    serialize_header(&req_header, header_buf);
    
    /* Send request */
    int sent = tcp_send(conn, header_buf, MESSAGE_HEADER_SIZE);
    if (sent != MESSAGE_HEADER_SIZE) {
        LOG_ERROR(client->logger, "rpc_client", "Failed to send header");
        result = DISTRIC_ERR_INIT_FAILED;
        goto cleanup;
    }
    
    if (request && req_len > 0) {
        sent = tcp_send(conn, request, req_len);
        if (sent != (int)req_len) {
            LOG_ERROR(client->logger, "rpc_client", "Failed to send payload");
            result = DISTRIC_ERR_INIT_FAILED;
            goto cleanup;
        }
    }
    
    /* Receive response header */
    uint8_t resp_header_buf[MESSAGE_HEADER_SIZE];
    int received = tcp_recv(conn, resp_header_buf, MESSAGE_HEADER_SIZE, timeout_ms);
    
    if (received != MESSAGE_HEADER_SIZE) {
        LOG_ERROR(client->logger, "rpc_client", "Failed to receive response header",
                 "received", (char*)&received);
        result = DISTRIC_ERR_INIT_FAILED;
        goto cleanup;
    }
    
    /* Deserialize response header */
    message_header_t resp_header;
    deserialize_header(resp_header_buf, &resp_header);
    
    /* Validate response */
    if (!validate_message_header(&resp_header)) {
        LOG_ERROR(client->logger, "rpc_client", "Invalid response header");
        result = DISTRIC_ERR_INVALID_FORMAT;
        goto cleanup;
    }
    
    if (resp_header.message_id != req_header.message_id) {
        LOG_ERROR(client->logger, "rpc_client", "Message ID mismatch");
        result = DISTRIC_ERR_INVALID_FORMAT;
        goto cleanup;
    }
    
    /* Receive response payload */
    uint8_t* response = NULL;
    if (resp_header.payload_len > 0) {
        response = (uint8_t*)malloc(resp_header.payload_len);
        if (!response) {
            result = DISTRIC_ERR_NO_MEMORY;
            goto cleanup;
        }
        
        received = tcp_recv(conn, response, resp_header.payload_len, timeout_ms);
        if (received != (int)resp_header.payload_len) {
            LOG_ERROR(client->logger, "rpc_client", "Failed to receive response payload");
            free(response);
            result = DISTRIC_ERR_INIT_FAILED;
            goto cleanup;
        }
    }
    
    /* Verify CRC */
    if (!verify_message_crc32(&resp_header, response, resp_header.payload_len)) {
        LOG_ERROR(client->logger, "rpc_client", "Response CRC32 verification failed");
        free(response);
        result = DISTRIC_ERR_INVALID_FORMAT;
        goto cleanup;
    }
    
    *response_out = response;
    *resp_len_out = resp_header.payload_len;
    
cleanup:
    /* Release connection back to pool */
    if (conn) {
        tcp_pool_release(client->tcp_pool, conn);
    }
    
    /* Update metrics */
    uint64_t duration_us = get_timestamp_us() - start_time;
    metrics_histogram_observe(client->call_duration, duration_us / 1000000.0);
    metrics_counter_inc(client->calls_total);
    
    if (result != DISTRIC_OK) {
        metrics_counter_inc(client->errors_total);
    }
    
    /* Finish trace span */
    if (span) {
        trace_set_status(span, result == DISTRIC_OK ? SPAN_STATUS_OK : SPAN_STATUS_ERROR);
        trace_finish_span(client->tracer, span);
    }
    
    LOG_DEBUG(client->logger, "rpc_client", "RPC call completed",
             "msg_type", message_type_to_string(msg_type),
             "duration_ms", (char*)&duration_us,
             "result", (char*)&result);
    
    return result;
}

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
) {
    distric_err_t result;
    
    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            /* Exponential backoff */
            int backoff_ms = DEFAULT_RETRY_BACKOFF_MS * (1 << (attempt - 1));
            usleep(backoff_ms * 1000);
            
            metrics_counter_inc(client->retries_total);
            
            LOG_INFO(client->logger, "rpc_client", "Retrying RPC call",
                    "attempt", (char*)&attempt,
                    "backoff_ms", (char*)&backoff_ms);
        }
        
        result = rpc_call(client, host, port, msg_type, request, req_len,
                         response_out, resp_len_out, timeout_ms);
        
        if (result == DISTRIC_OK) {
            return DISTRIC_OK;
        }
    }
    
    LOG_ERROR(client->logger, "rpc_client", "RPC call failed after retries",
             "max_retries", (char*)&max_retries);
    
    return result;
}

void rpc_client_destroy(rpc_client_t* client) {
    if (!client) {
        return;
    }
    
    free(client);
}