/**
 * @file rpc.c
 * @brief RPC Framework Implementation - FIXED CONNECTION HANDLER
 * 
 * CRITICAL FIX:
 * Handler now loops to process multiple requests on the same connection.
 * This matches the connection pooling behavior where connections are reused.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_protocol/rpc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>

/* ============================================================================
 * RPC SERVER
 * ========================================================================= */

#define MAX_HANDLERS 64

typedef struct {
    uint16_t msg_type;
    rpc_handler_fn_t handler;
    void* userdata;
} handler_entry_t;

struct rpc_server {
    tcp_server_t* tcp_server;
    
    handler_entry_t handlers[MAX_HANDLERS];
    size_t handler_count;
    pthread_rwlock_t handlers_lock;
    
    /* Graceful shutdown tracking */
    pthread_mutex_t active_handlers_lock;
    pthread_cond_t all_handlers_done;
    size_t active_handlers_count;
    volatile bool accepting_requests;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    tracer_t* tracer;
    
    metric_t* requests_total;
    metric_t* errors_total;
    metric_t* latency_metric;
};

struct rpc_client {
    tcp_pool_t* tcp_pool;
    
    metrics_registry_t* metrics;
    logger_t* logger;
    tracer_t* tracer;
    
    metric_t* calls_total;
    metric_t* errors_total;
    metric_t* latency_metric;
};

/* ============================================================================
 * RPC CONNECTION HANDLER
 * ========================================================================= */

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * ✓ CRITICAL FIX: Handler now LOOPS to process multiple requests
 * 
 * This matches the connection pooling behavior where connections are reused.
 * Previous implementation processed ONE request and exited, leaving connection
 * alive but with no thread to read subsequent requests.
 */
static void rpc_server_handle_connection(tcp_connection_t* conn, void* userdata) {
    rpc_server_t* server = (rpc_server_t*)userdata;
    
    /* Track active handler */
    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count++;
    pthread_mutex_unlock(&server->active_handlers_lock);
    
    /* Check if server is shutting down */
    if (!server->accepting_requests) {
        LOG_DEBUG(server->logger, "rpc_server", "Rejecting connection - server shutting down");
        goto cleanup_and_exit;
    }
    
    /* ✓ CRITICAL FIX: Process multiple requests in a loop */
    while (server->accepting_requests) {
        uint64_t start_time = get_time_us();
        trace_span_t* span = NULL;
        
        /* Start trace span if tracer available */
        if (server->tracer) {
            trace_start_span(server->tracer, "rpc_server_handle_request", &span);
        }
        
        /* Receive header with short timeout to allow checking shutdown flag */
        message_header_t header;
        uint8_t header_buf[MESSAGE_HEADER_SIZE];
        
        int received = tcp_recv(conn, header_buf, MESSAGE_HEADER_SIZE, 1000); /* 1s timeout */
        
        if (received == 0) {
            /* Connection closed by peer - normal exit */
            if (span) {
                trace_set_status(span, SPAN_STATUS_OK);
                trace_finish_span(server->tracer, span);
            }
            break;
        }
        
        if (received < 0) {
            if (received == -ETIMEDOUT) {
                /* Timeout - check if server is shutting down, otherwise continue */
                if (span) trace_finish_span(server->tracer, span);
                
                if (!server->accepting_requests) {
                    break;
                }
                continue;
            }
            
            /* Real error - close connection */
            LOG_ERROR(server->logger, "rpc_server", "Failed to receive header");
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }
        
        if (received != MESSAGE_HEADER_SIZE) {
            LOG_ERROR(server->logger, "rpc_server", "Incomplete header");
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }
        
        /* Deserialize header */
        deserialize_header(header_buf, &header);
        
        /* Validate header */
        if (!validate_message_header(&header)) {
            LOG_ERROR(server->logger, "rpc_server", "Invalid message header");
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }
        
        /* Receive payload */
        uint8_t* payload = NULL;
        if (header.payload_len > 0) {
            payload = (uint8_t*)malloc(header.payload_len);
            if (!payload) {
                LOG_ERROR(server->logger, "rpc_server", "Failed to allocate payload buffer");
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
            
            received = tcp_recv(conn, payload, header.payload_len, 5000); /* 5s timeout */
            if (received != (int)header.payload_len) {
                LOG_ERROR(server->logger, "rpc_server", "Failed to receive payload");
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                free(payload);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
        }
        
        /* Verify CRC32 */
        if (!verify_message_crc32(&header, payload, header.payload_len)) {
            LOG_ERROR(server->logger, "rpc_server", "CRC32 verification failed");
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            free(payload);
            if (span) trace_finish_span(server->tracer, span);
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
            
            /* Only log if server is still accepting requests */
            if (server->accepting_requests) {
                LOG_WARN(server->logger, "rpc_server", "No handler registered",
                        "msg_type", message_type_to_string(header.msg_type));
            }
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            free(payload);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }
        
        /* Call handler */
        uint8_t* response = NULL;
        size_t resp_len = 0;
        
        int result = handler_entry->handler(payload, header.payload_len, 
                                           &response, &resp_len,
                                           handler_entry->userdata, span);
        
        pthread_rwlock_unlock(&server->handlers_lock);
        
        free(payload);
        
        /* Send response */
        if (result == 0 && response) {
            /* Create response header */
            message_header_t resp_header;
            message_header_init(&resp_header, header.msg_type, resp_len);
            resp_header.flags = MSG_FLAG_RESPONSE;
            resp_header.message_id = header.message_id;
            
            /* Compute CRC32 */
            compute_header_crc32(&resp_header, response, resp_len);
            
            /* Serialize response header */
            uint8_t resp_header_buf[MESSAGE_HEADER_SIZE];
            serialize_header(&resp_header, resp_header_buf);
            
            /* Send header + payload */
            int sent = tcp_send(conn, resp_header_buf, MESSAGE_HEADER_SIZE);
            if (sent > 0 && resp_len > 0) {
                tcp_send(conn, response, resp_len);
            }
            
            free(response);
        }
        
        /* Record metrics */
        if (server->requests_total) {
            metrics_counter_inc(server->requests_total);
        }
        
        uint64_t duration_us = get_time_us() - start_time;
        if (server->latency_metric) {
            metrics_histogram_observe(server->latency_metric, (double)duration_us / 1000.0);
        }
        
        char duration_str[32];
        snprintf(duration_str, sizeof(duration_str), "%llu", (unsigned long long)(duration_us / 1000));
        LOG_DEBUG(server->logger, "rpc_server", "Request handled",
                 "msg_type", message_type_to_string(header.msg_type),
                 "duration_ms", duration_str);
        
        /* Finish trace span */
        if (span) {
            trace_set_status(span, SPAN_STATUS_OK);
            trace_finish_span(server->tracer, span);
        }
    } /* End of request processing loop */

cleanup_and_exit:
    /* Close connection when done */
    tcp_close(conn);
    
    /* Decrement active handler count and signal if last one */
    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count--;
    if (server->active_handlers_count == 0) {
        pthread_cond_signal(&server->all_handlers_done);
    }
    pthread_mutex_unlock(&server->active_handlers_lock);
}

/* ============================================================================
 * RPC SERVER LIFECYCLE
 * ========================================================================= */

distric_err_t rpc_server_create(
    tcp_server_t* tcp_server,
    metrics_registry_t* metrics,
    logger_t* logger,
    tracer_t* tracer,
    rpc_server_t** server_out
) {
    if (!tcp_server || !server_out) {
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
    server->active_handlers_count = 0;
    server->accepting_requests = true;
    
    pthread_rwlock_init(&server->handlers_lock, NULL);
    pthread_mutex_init(&server->active_handlers_lock, NULL);
    pthread_cond_init(&server->all_handlers_done, NULL);
    
    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "rpc_server_requests_total",
                                 "Total RPC requests handled", NULL, 0,
                                 &server->requests_total);
        metrics_register_counter(metrics, "rpc_server_errors_total",
                                 "Total RPC errors", NULL, 0,
                                 &server->errors_total);
        metrics_register_histogram(metrics, "rpc_server_latency_ms",
                                   "RPC request latency", NULL, 0,
                                   &server->latency_metric);
    }
    
    *server_out = server;
    
    return DISTRIC_OK;
}

void rpc_server_destroy(rpc_server_t* server) {
    if (!server) return;
    
    pthread_rwlock_destroy(&server->handlers_lock);
    pthread_mutex_destroy(&server->active_handlers_lock);
    pthread_cond_destroy(&server->all_handlers_done);
    
    free(server);
}

distric_err_t rpc_server_start(rpc_server_t* server) {
    if (!server) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    server->accepting_requests = true;
    
    distric_err_t err = tcp_server_start(server->tcp_server, 
                                         rpc_server_handle_connection, 
                                         server);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    LOG_INFO(server->logger, "rpc_server", "RPC server started");
    
    return DISTRIC_OK;
}

void rpc_server_stop(rpc_server_t* server) {
    if (!server) {
        return;
    }
    
    LOG_INFO(server->logger, "rpc_server", "RPC server stopping");
    
    /* Stop accepting new requests */
    server->accepting_requests = false;
    
    /* Stop TCP server (stops accepting new connections) */
    tcp_server_stop(server->tcp_server);
    
    /* Wait for all active handlers to complete with timeout */
    pthread_mutex_lock(&server->active_handlers_lock);
    
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;  /* 5 second timeout */
    
    while (server->active_handlers_count > 0) {
        int result = pthread_cond_timedwait(&server->all_handlers_done, 
                                           &server->active_handlers_lock,
                                           &timeout);
        if (result == ETIMEDOUT) {
            char count_str[32];
            snprintf(count_str, sizeof(count_str), "%zu", server->active_handlers_count);
            LOG_WARN(server->logger, "rpc_server", 
                    "Timeout waiting for handlers to complete",
                    "active_count", count_str);
            break;
        }
    }
    
    pthread_mutex_unlock(&server->active_handlers_lock);
    
    LOG_INFO(server->logger, "rpc_server", "RPC server stopped");
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
    
    server->handlers[server->handler_count].msg_type = msg_type;
    server->handlers[server->handler_count].handler = handler;
    server->handlers[server->handler_count].userdata = userdata;
    server->handler_count++;
    
    pthread_rwlock_unlock(&server->handlers_lock);
    
    LOG_DEBUG(server->logger, "rpc_server", "Handler registered",
             "msg_type", message_type_to_string(msg_type));
    
    return DISTRIC_OK;
}

/* ============================================================================
 * RPC CLIENT
 * ========================================================================= */

distric_err_t rpc_client_create(
    tcp_pool_t* tcp_pool,
    metrics_registry_t* metrics,
    logger_t* logger,
    tracer_t* tracer,
    rpc_client_t** client_out
) {
    if (!tcp_pool || !client_out) {
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
    
    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "rpc_client_calls_total",
                                 "Total RPC calls made", NULL, 0,
                                 &client->calls_total);
        metrics_register_counter(metrics, "rpc_client_errors_total",
                                 "Total RPC errors", NULL, 0,
                                 &client->errors_total);
        metrics_register_histogram(metrics, "rpc_client_latency_ms",
                                   "RPC call latency", NULL, 0,
                                   &client->latency_metric);
    }
    
    *client_out = client;
    
    return DISTRIC_OK;
}

void rpc_client_destroy(rpc_client_t* client) {
    if (!client) return;
    free(client);
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
    
    uint64_t start_time = get_time_us();
    
    /* Acquire connection */
    tcp_connection_t* conn = NULL;
    distric_err_t err = tcp_pool_acquire(client->tcp_pool, host, port, &conn);
    if (err != DISTRIC_OK) {
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return err;
    }
    
    /* Create request header */
    message_header_t req_header;
    message_header_init(&req_header, msg_type, req_len);
    compute_header_crc32(&req_header, request, req_len);
    
    /* Serialize header */
    uint8_t req_header_buf[MESSAGE_HEADER_SIZE];
    serialize_header(&req_header, req_header_buf);
    
    /* Send request */
    err = tcp_send(conn, req_header_buf, MESSAGE_HEADER_SIZE);
    if (err < 0) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    if (req_len > 0) {
        err = tcp_send(conn, request, req_len);
        if (err < 0) {
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    /* Receive response header */
    uint8_t resp_header_buf[MESSAGE_HEADER_SIZE];
    int received = tcp_recv(conn, resp_header_buf, MESSAGE_HEADER_SIZE, timeout_ms);
    if (received != MESSAGE_HEADER_SIZE) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Deserialize response header */
    message_header_t resp_header;
    deserialize_header(resp_header_buf, &resp_header);
    
    /* Validate response header */
    if (!validate_message_header(&resp_header)) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    /* Receive response payload */
    uint8_t* response = NULL;
    if (resp_header.payload_len > 0) {
        response = (uint8_t*)malloc(resp_header.payload_len);
        if (!response) {
            tcp_pool_release(client->tcp_pool, conn);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_NO_MEMORY;
        }
        
        received = tcp_recv(conn, response, resp_header.payload_len, timeout_ms);
        if (received != (int)resp_header.payload_len) {
            free(response);
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }
    
    /* Verify CRC32 */
    if (!verify_message_crc32(&resp_header, response, resp_header.payload_len)) {
        free(response);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    /* Release connection back to pool */
    tcp_pool_release(client->tcp_pool, conn);
    
    /* Record metrics */
    if (client->calls_total) {
        metrics_counter_inc(client->calls_total);
    }
    
    uint64_t duration_us = get_time_us() - start_time;
    if (client->latency_metric) {
        metrics_histogram_observe(client->latency_metric, (double)duration_us / 1000.0);
    }
    
    char duration_str[32];
    snprintf(duration_str, sizeof(duration_str), "%llu", (unsigned long long)(duration_us / 1000));
    LOG_DEBUG(client->logger, "rpc_client", "RPC call completed",
             "msg_type", message_type_to_string(msg_type),
             "duration_ms", duration_str);
    
    *response_out = response;
    *resp_len_out = resp_header.payload_len;
    
    return DISTRIC_OK;
}