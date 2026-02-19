/**
 * @file rpc.c
 * @brief RPC Framework - Production implementation
 *
 * Improvements implemented:
 *  - P0: Maximum payload size enforcement (RPC_MAX_MESSAGE_SIZE)
 *  - P0: Backpressure propagation (DISTRIC_ERR_BACKPRESSURE checked on sends)
 *  - P0: Proper DISTRIC_ERR_TIMEOUT returned (not generic INIT_FAILED)
 *  - Admission control: atomic active_requests counter + max_inflight limit
 *  - Graceful drain: configurable drain_timeout_ms
 *  - Structured error taxonomy: rpc_error_class_t
 *  - New metrics: rejected_overload, rejected_payload, timeout, backpressure
 *  - Protocol version check in server-side header validation
 *  - rpc_server_create_with_config() for tunable configuration
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
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>

/* ============================================================================
 * INTERNAL CONSTANTS
 * ========================================================================= */

#define MAX_HANDLERS        64
#define RECV_LOOP_TIMEOUT_MS 1000   /* Poll granularity while waiting for header */

/* ============================================================================
 * STRUCTURED ERROR TAXONOMY
 * ========================================================================= */

const char* rpc_error_class_to_string(rpc_error_class_t cls) {
    switch (cls) {
        case RPC_ERR_CLASS_OK:           return "ok";
        case RPC_ERR_CLASS_TIMEOUT:      return "timeout";
        case RPC_ERR_CLASS_UNAVAILABLE:  return "unavailable";
        case RPC_ERR_CLASS_OVERLOADED:   return "overloaded";
        case RPC_ERR_CLASS_BACKPRESSURE: return "backpressure";
        case RPC_ERR_CLASS_INVALID:      return "invalid";
        case RPC_ERR_CLASS_INTERNAL:     return "internal";
        default:                         return "unknown";
    }
}

rpc_error_class_t rpc_error_classify(distric_err_t err) {
    switch (err) {
        case DISTRIC_OK:
            return RPC_ERR_CLASS_OK;
        case DISTRIC_ERR_TIMEOUT:
            return RPC_ERR_CLASS_TIMEOUT;
        case DISTRIC_ERR_BACKPRESSURE:
            return RPC_ERR_CLASS_BACKPRESSURE;
        case DISTRIC_ERR_UNAVAILABLE:
        case DISTRIC_ERR_IO:
        case DISTRIC_ERR_EOF:
        case DISTRIC_ERR_INIT_FAILED:   /* connection-level failures */
            return RPC_ERR_CLASS_UNAVAILABLE;
        case DISTRIC_ERR_INVALID_ARG:
        case DISTRIC_ERR_INVALID_FORMAT:
        case DISTRIC_ERR_TYPE_MISMATCH:
        case DISTRIC_ERR_INVALID_STATE:
            return RPC_ERR_CLASS_INVALID;
        case DISTRIC_ERR_NO_MEMORY:
        case DISTRIC_ERR_BUFFER_OVERFLOW:
            return RPC_ERR_CLASS_INTERNAL;
        default:
            return RPC_ERR_CLASS_INTERNAL;
    }
}

/* ============================================================================
 * INTERNAL HANDLER REGISTRY ENTRY
 * ========================================================================= */

typedef struct {
    uint16_t      msg_type;
    rpc_handler_t handler;
    void*         userdata;
} handler_entry_t;

/* ============================================================================
 * RPC SERVER STRUCT
 * ========================================================================= */

struct rpc_server {
    tcp_server_t* tcp_server;

    /* Handler registry */
    handler_entry_t   handlers[MAX_HANDLERS];
    size_t            handler_count;
    pthread_rwlock_t  handlers_lock;

    /* Admission control (P0 + item 9) */
    atomic_uint_fast32_t active_requests;  /* current in-flight count */
    uint32_t             max_inflight_requests;

    /* Graceful drain (item 6) */
    pthread_mutex_t  active_handlers_lock;
    pthread_cond_t   all_handlers_done;
    size_t           active_handlers_count;  /* connection-level threads */
    volatile bool    accepting_requests;
    uint32_t         drain_timeout_ms;

    /* Payload limit */
    uint32_t max_message_size;

    /* Observability */
    metrics_registry_t* metrics;
    logger_t*           logger;
    tracer_t*           tracer;

    metric_t* requests_total;
    metric_t* errors_total;
    metric_t* latency_metric;
    metric_t* active_requests_gauge;
    metric_t* rejected_overload_total;  /* admission control drops */
    metric_t* rejected_payload_total;   /* payload-too-large drops */
};

/* ============================================================================
 * RPC CLIENT STRUCT
 * ========================================================================= */

struct rpc_client {
    tcp_pool_t*         tcp_pool;
    metrics_registry_t* metrics;
    logger_t*           logger;
    tracer_t*           tracer;

    metric_t* calls_total;
    metric_t* errors_total;
    metric_t* latency_metric;
    metric_t* retries_total;
    metric_t* timeout_total;
    metric_t* backpressure_total;
};

/* ============================================================================
 * TIME HELPER
 * ========================================================================= */

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ============================================================================
 * ERROR HELPERS
 * ========================================================================= */

/**
 * Translate a tcp_recv() return value to a distric_err_t.
 * tcp_recv returns:
 *  > 0        bytes received
 *  0          connection closed by peer (EOF)
 *  -ETIMEDOUT timeout
 *  < 0        other error
 */
/**
 * Map a tcp_recv() return value to a distric_err_t.
 *
 * tcp_recv return semantics (as observed from the transport):
 *   > 0          : bytes received
 *   0            : no bytes — two possible causes:
 *                    (a) peer closed the connection cleanly (EOF), OR
 *                    (b) the timeout window expired before any data arrived.
 *                  We disambiguate using timeout_ms: if a deadline was set,
 *                  the peer is expected to still be alive, so 0 means the
 *                  deadline fired (TIMEOUT).  Without a deadline the only
 *                  valid explanation is clean EOF.
 *   -ETIMEDOUT   : explicit timeout signal (some transport implementations)
 *   < 0 (other)  : I/O error
 *
 * @param rc          Raw return value from tcp_recv().
 * @param timeout_ms  The timeout that was passed to tcp_recv() (0 = no limit).
 */
static distric_err_t translate_recv_error(int rc, int timeout_ms) {
    if (rc == -ETIMEDOUT)              return DISTRIC_ERR_TIMEOUT;
    if (rc == 0 && timeout_ms > 0)    return DISTRIC_ERR_TIMEOUT;
    if (rc == 0)                       return DISTRIC_ERR_EOF;
    return DISTRIC_ERR_IO;
}

/* ============================================================================
 * SERVER CONNECTION HANDLER
 * ========================================================================= */

/**
 * Called by tcp_server on each new accepted connection.
 * Loops processing requests on the same connection until the connection
 * closes or the server stops accepting.
 *
 * Admission control: increments/decrements active_requests atomically.
 * Payload limit: rejects (and closes) connections that send oversized payloads.
 * Backpressure: on DISTRIC_ERR_BACKPRESSURE from tcp_send, logs and breaks
 *               (the client will reconnect and retry if it chooses to).
 */
static void rpc_server_handle_connection(tcp_connection_t* conn, void* userdata) {
    rpc_server_t* server = (rpc_server_t*)userdata;

    /* --- Track live connection thread --- */
    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count++;
    pthread_mutex_unlock(&server->active_handlers_lock);

    /* Reject immediately if draining */
    if (!server->accepting_requests) {
        LOG_DEBUG(server->logger, "rpc_server",
                  "Rejecting connection - server shutting down", NULL);
        goto cleanup_and_exit;
    }

    /* ------------------------------------------------------------------ */
    /* Request processing loop — one iteration per request on this conn   */
    /* ------------------------------------------------------------------ */
    while (server->accepting_requests) {

        uint64_t     start_time = get_time_us();
        trace_span_t* span      = NULL;

        if (server->tracer) {
            trace_start_span(server->tracer, "rpc_server_handle_request", &span);
        }

        /* ---- 1. Receive fixed-size header ---- */
        uint8_t      header_buf[MESSAGE_HEADER_SIZE];
        message_header_t header;

        int received = tcp_recv(conn, header_buf, MESSAGE_HEADER_SIZE,
                                RECV_LOOP_TIMEOUT_MS);

        if (received == 0) {
            /* Clean EOF — peer closed the connection */
            if (span) { trace_set_status(span, SPAN_STATUS_OK);
                        trace_finish_span(server->tracer, span); }
            break;
        }

        if (received < 0) {
            if (received == -ETIMEDOUT) {
                /* Poll timeout — check shutdown flag and loop back */
                if (span) trace_finish_span(server->tracer, span);
                if (!server->accepting_requests) break;
                continue;
            }
            /* Real I/O error */
            LOG_WARN(server->logger, "rpc_server", "Header recv error", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        if (received != MESSAGE_HEADER_SIZE) {
            LOG_ERROR(server->logger, "rpc_server", "Incomplete header received", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 2. Deserialize + validate header ---- */
        deserialize_header(header_buf, &header);

        if (!validate_message_header(&header)) {
            LOG_ERROR(server->logger, "rpc_server", "Invalid message header", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 3. Payload size enforcement (P0 item 3) ---- */
        if (header.payload_len > server->max_message_size) {
            char len_str[32], limit_str[32];
            snprintf(len_str,   sizeof(len_str),   "%u", header.payload_len);
            snprintf(limit_str, sizeof(limit_str), "%u", server->max_message_size);
            LOG_ERROR(server->logger, "rpc_server",
                      "Payload exceeds maximum size — closing connection",
                      "payload_len", len_str, "max", limit_str, NULL);
            if (server->rejected_payload_total)
                metrics_counter_inc(server->rejected_payload_total);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break; /* close connection */
        }

        /* ---- 4. Admission control (item 9) ---- */
        uint32_t cur = atomic_fetch_add_explicit(
            &server->active_requests, 1, memory_order_acq_rel) + 1;

        if (cur > server->max_inflight_requests) {
            /* Reject: send a bare error header back if possible */
            atomic_fetch_sub_explicit(&server->active_requests, 1,
                                      memory_order_release);
            LOG_WARN(server->logger, "rpc_server", "Rejecting request: overloaded",
                     NULL);
            if (server->rejected_overload_total)
                metrics_counter_inc(server->rejected_overload_total);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            /* 
             * We break here (close conn) rather than continue because:
             *   a) We haven't drained the payload from the wire.
             *   b) Keeping the conn open while overloaded risks more queuing.
             */
            break;
        }

        /* ---- 5. Receive payload ---- */
        uint8_t* payload = NULL;
        if (header.payload_len > 0) {
            payload = (uint8_t*)malloc(header.payload_len);
            if (!payload) {
                atomic_fetch_sub_explicit(&server->active_requests, 1,
                                          memory_order_release);
                LOG_ERROR(server->logger, "rpc_server",
                          "Failed to allocate payload buffer", NULL);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }

            received = tcp_recv(conn, payload, header.payload_len, 5000);
            if (received != (int)header.payload_len) {
                free(payload);
                atomic_fetch_sub_explicit(&server->active_requests, 1,
                                          memory_order_release);
                distric_err_t rerr = translate_recv_error(received, 5000);
                char err_str[32];
                snprintf(err_str, sizeof(err_str), "%d", (int)rerr);
                LOG_ERROR(server->logger, "rpc_server",
                          "Failed to receive payload", "err", err_str, NULL);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
        }

        /* ---- 6. Verify CRC32 ---- */
        if (!verify_message_crc32(&header, payload, header.payload_len)) {
            free(payload);
            atomic_fetch_sub_explicit(&server->active_requests, 1,
                                      memory_order_release);
            LOG_ERROR(server->logger, "rpc_server", "CRC32 mismatch", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 7. Dispatch to handler ---- */
        pthread_rwlock_rdlock(&server->handlers_lock);
        handler_entry_t* entry = NULL;
        for (size_t i = 0; i < server->handler_count; i++) {
            if (server->handlers[i].msg_type == (uint16_t)header.msg_type) {
                entry = &server->handlers[i];
                break;
            }
        }

        if (!entry) {
            pthread_rwlock_unlock(&server->handlers_lock);
            atomic_fetch_sub_explicit(&server->active_requests, 1,
                                      memory_order_release);
            if (server->accepting_requests) {
                LOG_WARN(server->logger, "rpc_server", "No handler registered",
                         "msg_type", message_type_to_string(header.msg_type), NULL);
            }
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            free(payload);
            if (span) trace_finish_span(server->tracer, span);
            /* Continue: unknown message type is not fatal to the connection */
            continue;
        }

        uint8_t* response  = NULL;
        size_t   resp_len  = 0;
        int handler_result = entry->handler(payload, header.payload_len,
                                            &response, &resp_len,
                                            entry->userdata, span);
        pthread_rwlock_unlock(&server->handlers_lock);
        free(payload);

        atomic_fetch_sub_explicit(&server->active_requests, 1,
                                  memory_order_release);

        /* ---- 8. Send response (backpressure-aware) ---- */
        if (handler_result == 0 && response != NULL) {
            message_header_t resp_hdr;
            message_header_init(&resp_hdr, header.msg_type, resp_len);
            resp_hdr.flags      = MSG_FLAG_RESPONSE;
            resp_hdr.message_id = header.message_id;
            compute_header_crc32(&resp_hdr, response, resp_len);

            uint8_t resp_hdr_buf[MESSAGE_HEADER_SIZE];
            serialize_header(&resp_hdr, resp_hdr_buf);

            /* P0 backpressure: respect DISTRIC_ERR_BACKPRESSURE */
            distric_err_t send_err = tcp_send(conn, resp_hdr_buf, MESSAGE_HEADER_SIZE);
            if (send_err == DISTRIC_ERR_BACKPRESSURE) {
                LOG_WARN(server->logger, "rpc_server",
                         "Send queue full (backpressure) — closing connection", NULL);
                free(response);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
            if (send_err < 0) {
                free(response);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }

            if (resp_len > 0) {
                send_err = tcp_send(conn, response, resp_len);
                if (send_err == DISTRIC_ERR_BACKPRESSURE) {
                    LOG_WARN(server->logger, "rpc_server",
                             "Send queue full on payload — closing connection", NULL);
                    free(response);
                    if (server->errors_total) metrics_counter_inc(server->errors_total);
                    if (span) trace_finish_span(server->tracer, span);
                    break;
                }
            }
            free(response);
        }

        /* ---- 9. Metrics ---- */
        if (server->requests_total)
            metrics_counter_inc(server->requests_total);

        uint64_t duration_us = get_time_us() - start_time;
        if (server->latency_metric)
            metrics_histogram_observe(server->latency_metric,
                                      (double)duration_us / 1000.0);

        char dur_str[32];
        snprintf(dur_str, sizeof(dur_str), "%llu",
                 (unsigned long long)(duration_us / 1000));
        LOG_DEBUG(server->logger, "rpc_server", "Request handled",
                  "msg_type", message_type_to_string(header.msg_type),
                  "duration_ms", dur_str, NULL);

        if (span) {
            trace_set_status(span, SPAN_STATUS_OK);
            trace_finish_span(server->tracer, span);
        }

    } /* end request loop */

cleanup_and_exit:
    tcp_close(conn);

    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count--;
    if (server->active_handlers_count == 0)
        pthread_cond_signal(&server->all_handlers_done);
    pthread_mutex_unlock(&server->active_handlers_lock);
}

/* ============================================================================
 * SERVER LIFECYCLE
 * ========================================================================= */

static distric_err_t rpc_server_alloc(
    tcp_server_t*            tcp_server,
    metrics_registry_t*      metrics,
    logger_t*                logger,
    tracer_t*                tracer,
    const rpc_server_config_t* cfg,
    rpc_server_t**           out
) {
    if (!tcp_server || !out) return DISTRIC_ERR_INVALID_ARG;

    rpc_server_t* s = (rpc_server_t*)calloc(1, sizeof(rpc_server_t));
    if (!s) return DISTRIC_ERR_NO_MEMORY;

    s->tcp_server         = tcp_server;
    s->metrics            = metrics;
    s->logger             = logger;
    s->tracer             = tracer;
    s->handler_count      = 0;
    s->active_handlers_count = 0;
    s->accepting_requests = true;

    /* Apply configuration (or defaults) */
    uint32_t max_inflight  = RPC_DEFAULT_MAX_INFLIGHT;
    uint32_t drain_ms      = RPC_DEFAULT_DRAIN_TIMEOUT_MS;
    uint32_t max_msg_size  = RPC_MAX_MESSAGE_SIZE;

    if (cfg) {
        if (cfg->max_inflight_requests > 0) max_inflight = cfg->max_inflight_requests;
        if (cfg->drain_timeout_ms      > 0) drain_ms     = cfg->drain_timeout_ms;
        if (cfg->max_message_size      > 0) max_msg_size = cfg->max_message_size;
    }

    s->max_inflight_requests = max_inflight;
    s->drain_timeout_ms      = drain_ms;
    s->max_message_size      = max_msg_size;

    atomic_init(&s->active_requests, 0);

    pthread_rwlock_init(&s->handlers_lock, NULL);
    pthread_mutex_init(&s->active_handlers_lock, NULL);
    pthread_cond_init(&s->all_handlers_done, NULL);

    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "rpc_server_requests_total",
                                 "Total RPC requests handled", NULL, 0,
                                 &s->requests_total);
        metrics_register_counter(metrics, "rpc_server_errors_total",
                                 "Total RPC errors", NULL, 0,
                                 &s->errors_total);
        metrics_register_histogram(metrics, "rpc_server_latency_ms",
                                   "RPC request latency (ms)", NULL, 0,
                                   &s->latency_metric);
        metrics_register_gauge(metrics, "rpc_server_active_requests",
                               "Currently in-flight RPC requests", NULL, 0,
                               &s->active_requests_gauge);
        metrics_register_counter(metrics, "rpc_server_rejected_overload_total",
                                 "Requests rejected due to overload", NULL, 0,
                                 &s->rejected_overload_total);
        metrics_register_counter(metrics, "rpc_server_rejected_payload_total",
                                 "Requests rejected: payload too large", NULL, 0,
                                 &s->rejected_payload_total);
    }

    *out = s;
    return DISTRIC_OK;
}

distric_err_t rpc_server_create(
    tcp_server_t*      tcp_server,
    metrics_registry_t* metrics,
    logger_t*          logger,
    tracer_t*          tracer,
    rpc_server_t**     server_out
) {
    return rpc_server_alloc(tcp_server, metrics, logger, tracer, NULL, server_out);
}

distric_err_t rpc_server_create_with_config(
    tcp_server_t*            tcp_server,
    metrics_registry_t*      metrics,
    logger_t*                logger,
    tracer_t*                tracer,
    const rpc_server_config_t* config,
    rpc_server_t**           server_out
) {
    return rpc_server_alloc(tcp_server, metrics, logger, tracer, config, server_out);
}

distric_err_t rpc_server_register_handler(
    rpc_server_t*  server,
    message_type_t msg_type,
    rpc_handler_t  handler,
    void*          userdata
) {
    if (!server || !handler) return DISTRIC_ERR_INVALID_ARG;

    pthread_rwlock_wrlock(&server->handlers_lock);
    if (server->handler_count >= MAX_HANDLERS) {
        pthread_rwlock_unlock(&server->handlers_lock);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    server->handlers[server->handler_count].msg_type = (uint16_t)msg_type;
    server->handlers[server->handler_count].handler  = handler;
    server->handlers[server->handler_count].userdata = userdata;
    server->handler_count++;
    pthread_rwlock_unlock(&server->handlers_lock);

    LOG_DEBUG(server->logger, "rpc_server", "Handler registered",
              "msg_type", message_type_to_string(msg_type), NULL);
    return DISTRIC_OK;
}

distric_err_t rpc_server_start(rpc_server_t* server) {
    if (!server) return DISTRIC_ERR_INVALID_ARG;
    server->accepting_requests = true;

    distric_err_t err = tcp_server_start(server->tcp_server,
                                          rpc_server_handle_connection,
                                          server);
    if (err != DISTRIC_OK) return err;

    char limit_str[32], drain_str[32], maxmsg_str[32];
    snprintf(limit_str,  sizeof(limit_str),  "%u", server->max_inflight_requests);
    snprintf(drain_str,  sizeof(drain_str),  "%u", server->drain_timeout_ms);
    snprintf(maxmsg_str, sizeof(maxmsg_str), "%u", server->max_message_size);
    LOG_INFO(server->logger, "rpc_server", "RPC server started",
             "max_inflight", limit_str,
             "drain_ms",     drain_str,
             "max_msg_size", maxmsg_str, NULL);
    return DISTRIC_OK;
}

void rpc_server_stop(rpc_server_t* server) {
    if (!server) return;

    LOG_INFO(server->logger, "rpc_server", "RPC server stopping", NULL);

    /* Signal all request loops to exit at their next poll boundary */
    server->accepting_requests = false;

    /* Stop accepting new TCP connections */
    tcp_server_stop(server->tcp_server);

    /* Wait for active connection threads to complete */
    pthread_mutex_lock(&server->active_handlers_lock);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(server->drain_timeout_ms / 1000);
    deadline.tv_nsec += (long)((server->drain_timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }

    while (server->active_handlers_count > 0) {
        int rc = pthread_cond_timedwait(&server->all_handlers_done,
                                        &server->active_handlers_lock,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            char cnt_str[32];
            snprintf(cnt_str, sizeof(cnt_str), "%zu",
                     server->active_handlers_count);
            LOG_WARN(server->logger, "rpc_server",
                     "Drain timeout: handlers still active",
                     "count", cnt_str, NULL);
            break;
        }
    }
    pthread_mutex_unlock(&server->active_handlers_lock);
    LOG_INFO(server->logger, "rpc_server", "RPC server stopped", NULL);
}

void rpc_server_destroy(rpc_server_t* server) {
    if (!server) return;
    pthread_rwlock_destroy(&server->handlers_lock);
    pthread_mutex_destroy(&server->active_handlers_lock);
    pthread_cond_destroy(&server->all_handlers_done);
    free(server);
}

/* ============================================================================
 * RPC CLIENT
 * ========================================================================= */

distric_err_t rpc_client_create(
    tcp_pool_t*         tcp_pool,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tracer_t*           tracer,
    rpc_client_t**      client_out
) {
    if (!tcp_pool || !client_out) return DISTRIC_ERR_INVALID_ARG;

    rpc_client_t* c = (rpc_client_t*)calloc(1, sizeof(rpc_client_t));
    if (!c) return DISTRIC_ERR_NO_MEMORY;

    c->tcp_pool = tcp_pool;
    c->metrics  = metrics;
    c->logger   = logger;
    c->tracer   = tracer;

    if (metrics) {
        metrics_register_counter(metrics, "rpc_client_calls_total",
                                 "Total RPC calls made", NULL, 0, &c->calls_total);
        metrics_register_counter(metrics, "rpc_client_errors_total",
                                 "Total RPC errors", NULL, 0, &c->errors_total);
        metrics_register_histogram(metrics, "rpc_client_latency_ms",
                                   "RPC call latency (ms)", NULL, 0,
                                   &c->latency_metric);
        metrics_register_counter(metrics, "rpc_client_retries_total",
                                 "RPC retries", NULL, 0, &c->retries_total);
        metrics_register_counter(metrics, "rpc_client_timeout_total",
                                 "RPC calls that timed out", NULL, 0,
                                 &c->timeout_total);
        metrics_register_counter(metrics, "rpc_client_backpressure_total",
                                 "RPC calls rejected by backpressure", NULL, 0,
                                 &c->backpressure_total);
    }

    *client_out = c;
    return DISTRIC_OK;
}

void rpc_client_destroy(rpc_client_t* client) {
    if (!client) return;
    free(client);
}

/* ============================================================================
 * rpc_call — single attempt, full pipeline
 * ========================================================================= */

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
) {
    if (!client || !host || !response_out || !resp_len_out)
        return DISTRIC_ERR_INVALID_ARG;

    /* P0 payload limit — validate outgoing size too */
    if (req_len > RPC_MAX_MESSAGE_SIZE) {
        LOG_ERROR(client->logger, "rpc_client",
                  "Request payload exceeds RPC_MAX_MESSAGE_SIZE", NULL);
        return DISTRIC_ERR_INVALID_ARG;
    }

    uint64_t start_us = get_time_us();

    /* ---- Acquire connection ---- */
    tcp_connection_t* conn = NULL;
    distric_err_t err = tcp_pool_acquire(client->tcp_pool, host, port, &conn);
    if (err != DISTRIC_OK) {
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return err;
    }

    /* ---- Build + send request header ---- */
    message_header_t req_hdr;
    message_header_init(&req_hdr, msg_type, (uint32_t)req_len);
    compute_header_crc32(&req_hdr, request, req_len);

    uint8_t req_hdr_buf[MESSAGE_HEADER_SIZE];
    serialize_header(&req_hdr, req_hdr_buf);

    /* P0 backpressure: propagate DISTRIC_ERR_BACKPRESSURE to caller */
    err = tcp_send(conn, req_hdr_buf, MESSAGE_HEADER_SIZE);
    if (err == DISTRIC_ERR_BACKPRESSURE) {
        tcp_pool_release(client->tcp_pool, conn);
        if (client->backpressure_total) metrics_counter_inc(client->backpressure_total);
        if (client->errors_total)       metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_BACKPRESSURE;
    }
    if (err < 0) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_UNAVAILABLE;
    }

    if (req_len > 0) {
        err = tcp_send(conn, request, req_len);
        if (err == DISTRIC_ERR_BACKPRESSURE) {
            tcp_pool_release(client->tcp_pool, conn);
            if (client->backpressure_total) metrics_counter_inc(client->backpressure_total);
            if (client->errors_total)       metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_BACKPRESSURE;
        }
        if (err < 0) {
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_UNAVAILABLE;
        }
    }

    /* ---- Receive response header (with proper timeout mapping) ---- */
    uint8_t resp_hdr_buf[MESSAGE_HEADER_SIZE];
    int received = tcp_recv(conn, resp_hdr_buf, MESSAGE_HEADER_SIZE, timeout_ms);

    if (received != MESSAGE_HEADER_SIZE) {
        distric_err_t rerr = translate_recv_error(received, timeout_ms);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (rerr == DISTRIC_ERR_TIMEOUT && client->timeout_total)
            metrics_counter_inc(client->timeout_total);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return rerr;
    }

    /* ---- Validate response header ---- */
    message_header_t resp_hdr;
    deserialize_header(resp_hdr_buf, &resp_hdr);

    if (!validate_message_header(&resp_hdr)) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* P0 payload limit: do not allocate huge buffers from untrusted peers */
    if (resp_hdr.payload_len > RPC_MAX_MESSAGE_SIZE) {
        LOG_ERROR(client->logger, "rpc_client",
                  "Response payload exceeds RPC_MAX_MESSAGE_SIZE — closing connection",
                  NULL);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* ---- Receive response payload ---- */
    uint8_t* response = NULL;
    if (resp_hdr.payload_len > 0) {
        response = (uint8_t*)malloc(resp_hdr.payload_len);
        if (!response) {
            tcp_pool_release(client->tcp_pool, conn);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_NO_MEMORY;
        }

        received = tcp_recv(conn, response, resp_hdr.payload_len, timeout_ms);
        if (received != (int)resp_hdr.payload_len) {
            free(response);
            distric_err_t rerr = translate_recv_error(received, timeout_ms);
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (rerr == DISTRIC_ERR_TIMEOUT && client->timeout_total)
                metrics_counter_inc(client->timeout_total);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return rerr;
        }
    }

    /* ---- Verify CRC32 ---- */
    if (!verify_message_crc32(&resp_hdr, response, resp_hdr.payload_len)) {
        free(response);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* ---- Release healthy connection back to pool ---- */
    tcp_pool_release(client->tcp_pool, conn);

    /* ---- Record success metrics ---- */
    if (client->calls_total) metrics_counter_inc(client->calls_total);
    uint64_t dur_us = get_time_us() - start_us;
    if (client->latency_metric)
        metrics_histogram_observe(client->latency_metric, (double)dur_us / 1000.0);

    char dur_str[32];
    snprintf(dur_str, sizeof(dur_str), "%llu",
             (unsigned long long)(dur_us / 1000));
    LOG_DEBUG(client->logger, "rpc_client", "RPC call completed",
              "msg_type", message_type_to_string(msg_type),
              "duration_ms", dur_str, NULL);

    *response_out  = response;
    *resp_len_out  = resp_hdr.payload_len;
    return DISTRIC_OK;
}

/* ============================================================================
 * rpc_call_with_retry — backoff retry wrapper
 * ========================================================================= */

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
) {
    distric_err_t err = DISTRIC_OK;
    int           backoff_ms = 50;  /* initial backoff */

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            if (client->retries_total) metrics_counter_inc(client->retries_total);
            struct timespec ts = {
                .tv_sec  = backoff_ms / 1000,
                .tv_nsec = (backoff_ms % 1000) * 1000000L
            };
            nanosleep(&ts, NULL);
            backoff_ms = (backoff_ms * 2 < 5000) ? backoff_ms * 2 : 5000;
        }

        err = rpc_call(client, host, port, msg_type,
                       request, req_len,
                       response_out, resp_len_out,
                       timeout_ms);

        if (err == DISTRIC_OK) return DISTRIC_OK;

        /* Only retry on transient errors */
        rpc_error_class_t cls = rpc_error_classify(err);
        if (cls == RPC_ERR_CLASS_INVALID || cls == RPC_ERR_CLASS_INTERNAL)
            break; /* permanent — do not retry */
    }

    return err;
}