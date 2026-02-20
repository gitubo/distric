/**
 * @file rpc.c
 * @brief RPC Framework — Production implementation
 *
 * Applied improvements:
 *
 *  Improvement #2 — active_requests gauge kept in sync.
 *    server_request_inc() / server_request_dec() helpers now call
 *    metrics_gauge_set() after every atomic counter change so the
 *    rpc_server_active_requests Prometheus gauge reflects the true
 *    in-flight count rather than a permanent 0.
 *
 *  Improvement #3 — Response payload tcp_send error no longer silently ignored.
 *    The second tcp_send() (payload bytes) now checks its return code.  On
 *    failure it logs LOG_ERROR, increments errors_total, and breaks the
 *    connection loop so the client receives a clean EOF rather than a partial
 *    payload followed by a CRC mismatch.
 *
 *  Improvement #7 — Handler unregistration with tombstone support.
 *    rpc_server_unregister_handler() uses 0xFFFF as a tombstone sentinel.
 *    handler_table_find() skips tombstones.  handler_table_insert() recycles
 *    tombstone slots before declaring the table full.
 *
 *  Improvement #8 — Jittered exponential backoff in rpc_call_with_retry.
 *    Each retry delay uses ±25 % random jitter seeded from the current
 *    monotonic timestamp XOR the calling thread's ID, desynchronising
 *    simultaneous retries and preventing thundering-herd behaviour.
 *
 *  Improvement #9 — Total wall-clock timeout budget in rpc_call_with_retry.
 *    Reads total_timeout_ms from client config.  Trims per-attempt timeout to
 *    remaining budget.  Returns DISTRIC_ERR_TIMEOUT and increments
 *    rpc_client_budget_exceeded_total when the budget is exhausted before
 *    max_retries is reached.
 *
 * Pre-existing fixes (retained):
 *  FIX-A  — translate_recv_error uses DISTRIC_ERR_TIMEOUT, not raw errno.
 *  FIX-B  — Server header-recv loop recognises DISTRIC_ERR_TIMEOUT.
 *  FIX-B2 — Payload recv uses accumulating retry loop.
 *  FIX-C  — Correct teardown order: drain first, then tcp_server_stop().
 *  FIX-D  — DISTRIC_ERR_IO / INIT_FAILED map to RPC_ERR_CLASS_UNAVAILABLE.
 *  FIX-E  — LOG_ERROR on malloc failure and incomplete payload.
 *  Fix #4 — Pool starvation: sem_timedwait with pool_acquire_timeout_ms.
 *  Fix #5 — O(1) handler dispatch table; lock released before handler call.
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
#include <semaphore.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>

/* ============================================================================
 * INTERNAL CONSTANTS
 * ========================================================================= */

/** Poll granularity while waiting for a complete header (ms). */
#define RECV_LOOP_TIMEOUT_MS  1000

/**
 * Handler dispatch hash table.
 * HANDLER_TABLE_SIZE must be a power of 2.  64 slots → load factor < 0.33
 * for the typical ≤20 registered handlers.
 *
 * Sentinel values:
 *   msg_type == 0x0000 → empty slot  (validate_message_header rejects 0)
 *   msg_type == 0xFFFF → tombstone   (Improvement #7: marks deleted entries)
 */
#define HANDLER_TABLE_SIZE  64
#define HANDLER_TABLE_MASK  (HANDLER_TABLE_SIZE - 1)
#define HANDLER_TOMBSTONE   0xFFFFu

/** Minimum per-attempt timeout allowed when the budget is nearly exhausted. */
#define BUDGET_MIN_TIMEOUT_MS  50

/* ============================================================================
 * STRUCTURED ERROR TAXONOMY
 * ========================================================================= */

const char* rpc_error_class_to_string(rpc_error_class_t cls)
{
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

rpc_error_class_t rpc_error_classify(distric_err_t err)
{
    switch (err) {
        case DISTRIC_OK:               return RPC_ERR_CLASS_OK;
        case DISTRIC_ERR_TIMEOUT:      return RPC_ERR_CLASS_TIMEOUT;
        case DISTRIC_ERR_UNAVAILABLE:
        case DISTRIC_ERR_IO:
        case DISTRIC_ERR_INIT_FAILED:  return RPC_ERR_CLASS_UNAVAILABLE;
        case DISTRIC_ERR_BACKPRESSURE: return RPC_ERR_CLASS_BACKPRESSURE;
        case DISTRIC_ERR_INVALID_ARG:
        case DISTRIC_ERR_INVALID_FORMAT:
        case DISTRIC_ERR_TYPE_MISMATCH:return RPC_ERR_CLASS_INVALID;
        case DISTRIC_ERR_NO_MEMORY:    return RPC_ERR_CLASS_INTERNAL;
        default:                       return RPC_ERR_CLASS_INTERNAL;
    }
}

/* ============================================================================
 * TIME HELPERS
 * ========================================================================= */

static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void make_abs_deadline(struct timespec* deadline, uint32_t timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec  += (time_t)(timeout_ms / 1000);
    deadline->tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        deadline->tv_sec++;
    }
}

/* ============================================================================
 * ERROR TRANSLATION
 * ========================================================================= */

static distric_err_t translate_recv_error(int rc, int timeout_ms)
{
    if (rc == (int)DISTRIC_ERR_TIMEOUT)  return DISTRIC_ERR_TIMEOUT;
    if (rc == (int)DISTRIC_ERR_EOF)      return DISTRIC_ERR_EOF;
    if (rc == -ETIMEDOUT || rc == -EAGAIN || rc == -EWOULDBLOCK)
        return DISTRIC_ERR_TIMEOUT;
    if (rc == 0 && timeout_ms > 0)  return DISTRIC_ERR_TIMEOUT;
    if (rc == 0)                    return DISTRIC_ERR_EOF;
    return DISTRIC_ERR_IO;
}

/* ============================================================================
 * HANDLER TABLE (Improvement #7: tombstone support)
 * ========================================================================= */

typedef struct {
    uint16_t      msg_type;  /* 0x0000 = empty, 0xFFFF = tombstone */
    rpc_handler_t handler;
    void*         userdata;
} handler_slot_t;

/** Knuth multiplicative hash for 16-bit msg_type keys. */
static inline size_t handler_hash(uint16_t msg_type)
{
    return ((uint32_t)msg_type * 2654435769u) >> (32 - 6);
}

/**
 * @brief Look up a handler slot by msg_type.
 *
 * Skips tombstone entries (Improvement #7).
 * Returns NULL if not found.  Must be called with handlers_lock held for read.
 */
static const handler_slot_t* handler_table_find(
    const handler_slot_t* table, uint16_t msg_type)
{
    size_t idx = handler_hash(msg_type) & HANDLER_TABLE_MASK;

    for (size_t i = 0; i < HANDLER_TABLE_SIZE; i++) {
        const handler_slot_t* slot = &table[(idx + i) & HANDLER_TABLE_MASK];

        if (slot->msg_type == 0) return NULL;              /* empty → not found */
        if (slot->msg_type == HANDLER_TOMBSTONE) continue; /* skip tombstone */
        if (slot->msg_type == msg_type) return slot;
    }
    return NULL;
}

/**
 * @brief Insert or replace a handler in the hash table.
 *
 * Recycles tombstone slots before declaring the table full (Improvement #7).
 * Must be called with handlers_lock held for write.
 */
static distric_err_t handler_table_insert(
    handler_slot_t* table, uint16_t msg_type,
    rpc_handler_t handler, void* userdata)
{
    size_t idx = handler_hash(msg_type) & HANDLER_TABLE_MASK;
    size_t tombstone_idx = SIZE_MAX;

    for (size_t i = 0; i < HANDLER_TABLE_SIZE; i++) {
        size_t pos = (idx + i) & HANDLER_TABLE_MASK;
        handler_slot_t* slot = &table[pos];

        if (slot->msg_type == msg_type) {
            /* Replace existing */
            slot->handler  = handler;
            slot->userdata = userdata;
            return DISTRIC_OK;
        }
        if (slot->msg_type == HANDLER_TOMBSTONE && tombstone_idx == SIZE_MAX) {
            tombstone_idx = pos; /* remember first tombstone for recycling */
        }
        if (slot->msg_type == 0) {
            /* Empty slot — use it (or use the earlier tombstone if we saw one) */
            size_t target = (tombstone_idx != SIZE_MAX) ? tombstone_idx : pos;
            table[target].msg_type = msg_type;
            table[target].handler  = handler;
            table[target].userdata = userdata;
            return DISTRIC_OK;
        }
    }

    /* Table full — try to recycle tombstone if we found one */
    if (tombstone_idx != SIZE_MAX) {
        table[tombstone_idx].msg_type = msg_type;
        table[tombstone_idx].handler  = handler;
        table[tombstone_idx].userdata = userdata;
        return DISTRIC_OK;
    }

    return DISTRIC_ERR_REGISTRY_FULL;
}

/**
 * @brief Remove a handler by placing a tombstone in its slot.
 *
 * Improvement #7: zeroing the slot directly would break open-addressing probe
 * chains.  Using a tombstone preserves probe-chain continuity.
 * Must be called with handlers_lock held for write.
 *
 * @return DISTRIC_OK if found and tombstoned, DISTRIC_ERR_NOT_FOUND otherwise.
 */
static distric_err_t handler_table_remove(
    handler_slot_t* table, uint16_t msg_type)
{
    size_t idx = handler_hash(msg_type) & HANDLER_TABLE_MASK;

    for (size_t i = 0; i < HANDLER_TABLE_SIZE; i++) {
        handler_slot_t* slot = &table[(idx + i) & HANDLER_TABLE_MASK];

        if (slot->msg_type == 0) return DISTRIC_ERR_NOT_FOUND; /* empty → chain end */
        if (slot->msg_type == HANDLER_TOMBSTONE) continue;
        if (slot->msg_type == msg_type) {
            slot->msg_type = HANDLER_TOMBSTONE;
            slot->handler  = NULL;
            slot->userdata = NULL;
            return DISTRIC_OK;
        }
    }
    return DISTRIC_ERR_NOT_FOUND;
}

/* ============================================================================
 * RPC SERVER STRUCT
 * ========================================================================= */

struct rpc_server {
    tcp_server_t* tcp_server;

    /* Fix #5: O(1) hash dispatch table with tombstone support (Improvement #7) */
    handler_slot_t   handler_table[HANDLER_TABLE_SIZE];
    pthread_rwlock_t handlers_lock;

    /* Admission control */
    atomic_uint_fast32_t active_requests;
    uint32_t             max_inflight_requests;

    /* Graceful drain */
    pthread_mutex_t  active_handlers_lock;
    pthread_cond_t   all_handlers_done;
    size_t           active_handlers_count;
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
    metric_t* active_requests_gauge;   /* Improvement #2: kept in sync */
    metric_t* rejected_overload_total;
    metric_t* rejected_payload_total;
};

/* ============================================================================
 * IMPROVEMENT #2 — Active-requests helpers (counter + gauge in one call)
 * ========================================================================= */

/**
 * @brief Atomically increment active_requests and update the gauge.
 * @return New (post-increment) value.
 */
static inline uint32_t server_request_inc(rpc_server_t* server)
{
    uint32_t prev = (uint32_t)atomic_fetch_add_explicit(
        &server->active_requests, 1, memory_order_acquire);
    uint32_t current = prev + 1u;
    if (server->active_requests_gauge)
        metrics_gauge_set(server->active_requests_gauge, (double)current);
    return current;
}

/**
 * @brief Atomically decrement active_requests and update the gauge.
 */
static inline void server_request_dec(rpc_server_t* server)
{
    uint32_t prev = (uint32_t)atomic_fetch_sub_explicit(
        &server->active_requests, 1, memory_order_release);
    uint32_t current = (prev > 0) ? prev - 1u : 0u;
    if (server->active_requests_gauge)
        metrics_gauge_set(server->active_requests_gauge, (double)current);
}

/* ============================================================================
 * RPC CLIENT STRUCT
 * ========================================================================= */

struct rpc_client {
    tcp_pool_t*         tcp_pool;
    metrics_registry_t* metrics;
    logger_t*           logger;
    tracer_t*           tracer;

    /* Fix #4: concurrency semaphore */
    bool     use_sem;
    sem_t    acquire_sem;
    uint32_t pool_acquire_timeout_ms;

    /* Improvement #9: total wall-clock retry budget */
    uint32_t total_timeout_ms;

    metric_t* calls_total;
    metric_t* errors_total;
    metric_t* latency_metric;
    metric_t* retries_total;
    metric_t* timeout_total;
    metric_t* backpressure_total;
    metric_t* pool_timeout_total;
    metric_t* budget_exceeded_total;   /* Improvement #9 */
};

/* ============================================================================
 * SERVER CONNECTION HANDLER
 * ========================================================================= */

static void rpc_server_handle_connection(tcp_connection_t* conn, void* userdata)
{
    rpc_server_t* server = (rpc_server_t*)userdata;

    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count++;
    pthread_mutex_unlock(&server->active_handlers_lock);

    if (!server->accepting_requests) goto cleanup_and_exit;

    while (server->accepting_requests) {

        uint64_t      start_time = get_time_us();
        trace_span_t* span       = NULL;

        if (server->tracer)
            trace_start_span(server->tracer, "rpc_server_handle_request", &span);

        /* ---- 1. Receive header (accumulating loop, FIX-B) ---- */
        uint8_t          header_buf[MESSAGE_HEADER_SIZE];
        message_header_t header;
        size_t           hdr_recvd = 0;

        while (hdr_recvd < MESSAGE_HEADER_SIZE && server->accepting_requests) {
            int r = tcp_recv(conn,
                             header_buf + hdr_recvd,
                             MESSAGE_HEADER_SIZE - hdr_recvd,
                             RECV_LOOP_TIMEOUT_MS);

            if (r == 0 || r == (int)DISTRIC_ERR_EOF) goto clean_eof;
            if (r == (int)DISTRIC_ERR_TIMEOUT) continue;  /* FIX-B */
            if (r < 0) goto connection_error;
            hdr_recvd += (size_t)r;
        }

        if (hdr_recvd != MESSAGE_HEADER_SIZE) {
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 2. Deserialise & validate header ---- */
        if (deserialize_header(header_buf, &header) != DISTRIC_OK ||
            !validate_message_header(&header)) {
            LOG_WARN(server->logger, "rpc_server", "Invalid header received", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 3. Payload size enforcement ---- */
        if (header.payload_len > server->max_message_size) {
            LOG_WARN(server->logger, "rpc_server", "Payload too large", NULL);
            if (server->rejected_payload_total)
                metrics_counter_inc(server->rejected_payload_total);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 4. Admission control ---- */
        uint32_t current = server_request_inc(server); /* Improvement #2 */
        if (current > server->max_inflight_requests) {
            server_request_dec(server);                 /* Improvement #2 */
            LOG_WARN(server->logger, "rpc_server", "Max inflight exceeded", NULL);
            if (server->rejected_overload_total)
                metrics_counter_inc(server->rejected_overload_total);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        if (server->requests_total) metrics_counter_inc(server->requests_total);

        /* ---- 5. Receive payload (accumulating loop, FIX-B2) ---- */
        uint8_t* payload = NULL;
        if (header.payload_len > 0) {
            payload = (uint8_t*)malloc(header.payload_len);
            if (!payload) {
                LOG_ERROR(server->logger, "rpc_server",
                          "malloc failed for payload", NULL);
                server_request_dec(server);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }

            size_t bytes_recvd = 0;
            while (bytes_recvd < header.payload_len && server->accepting_requests) {
                int r = tcp_recv(conn,
                                 payload + bytes_recvd,
                                 header.payload_len - bytes_recvd,
                                 RECV_LOOP_TIMEOUT_MS);

                if (r == (int)DISTRIC_ERR_TIMEOUT) continue;   /* FIX-B2 */
                if (r == 0 || r == (int)DISTRIC_ERR_EOF) break;
                if (r < 0) break;
                bytes_recvd += (size_t)r;
            }

            if (bytes_recvd != header.payload_len) {
                if (bytes_recvd > 0) {
                    LOG_ERROR(server->logger, "rpc_server",
                              "Incomplete payload received", NULL);
                }
                free(payload);
                server_request_dec(server);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
        }

        /* ---- 6. CRC32 ---- */
        if (!verify_message_crc32(&header, payload, header.payload_len)) {
            free(payload);
            server_request_dec(server);
            LOG_ERROR(server->logger, "rpc_server", "CRC32 mismatch", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 7. Dispatch (Fix #5: lock released before handler call) ---- */
        pthread_rwlock_rdlock(&server->handlers_lock);
        const handler_slot_t* slot =
            handler_table_find(server->handler_table, (uint16_t)header.msg_type);
        rpc_handler_t handler_fn = slot ? slot->handler  : NULL;
        void*         handler_ud = slot ? slot->userdata : NULL;
        pthread_rwlock_unlock(&server->handlers_lock);

        if (!handler_fn) {
            server_request_dec(server);
            if (server->accepting_requests) {
                LOG_WARN(server->logger, "rpc_server", "No handler registered",
                         "msg_type",
                         message_type_to_string((message_type_t)header.msg_type),
                         NULL);
            }
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            free(payload);
            if (span) trace_finish_span(server->tracer, span);
            continue;
        }

        /* ---- 8. Call handler (lock NOT held) ---- */
        uint8_t* response  = NULL;
        size_t   resp_len  = 0;
        int handler_result = handler_fn(payload, header.payload_len,
                                        &response, &resp_len,
                                        handler_ud, span);
        free(payload);
        server_request_dec(server); /* Improvement #2 */

        /* ---- 9. Send response ---- */
        if (handler_result == 0) {
            message_header_t resp_hdr;
            message_header_init(&resp_hdr, (message_type_t)header.msg_type,
                                (uint32_t)resp_len);
            compute_header_crc32(&resp_hdr, response, resp_len);

            uint8_t resp_hdr_buf[MESSAGE_HEADER_SIZE];
            serialize_header(&resp_hdr, resp_hdr_buf);

            distric_err_t send_err = tcp_send(conn, resp_hdr_buf, MESSAGE_HEADER_SIZE);
            if (send_err == DISTRIC_ERR_BACKPRESSURE) {
                LOG_WARN(server->logger, "rpc_server",
                         "Backpressure on response header send — closing", NULL);
                free(response);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
            if (send_err != DISTRIC_OK) {
                LOG_ERROR(server->logger, "rpc_server",
                          "Failed to send response header", NULL);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                free(response);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }

            /* Improvement #3: check payload send return value */
            if (resp_len > 0) {
                distric_err_t psend_err = tcp_send(conn, response, resp_len);
                if (psend_err != DISTRIC_OK) {
                    LOG_ERROR(server->logger, "rpc_server",
                              "Failed to send response payload", NULL);
                    if (server->errors_total)
                        metrics_counter_inc(server->errors_total);
                    free(response);
                    if (span) trace_finish_span(server->tracer, span);
                    break;
                }
            }
        }
        free(response);

        /* ---- 10. Record latency ---- */
        if (server->latency_metric) {
            uint64_t dur_us = get_time_us() - start_time;
            metrics_histogram_observe(server->latency_metric,
                                      (double)dur_us / 1000.0);
        }

        if (span) {
            trace_set_status(span, SPAN_STATUS_OK);
            trace_finish_span(server->tracer, span);
        }
        continue;

    clean_eof:
        if (span) { trace_set_status(span, SPAN_STATUS_OK);
                    trace_finish_span(server->tracer, span); }
        break;

    connection_error:
        if (span) trace_finish_span(server->tracer, span);
        break;
    }

cleanup_and_exit:
    pthread_mutex_lock(&server->active_handlers_lock);
    if (server->active_handlers_count > 0) server->active_handlers_count--;
    if (server->active_handlers_count == 0)
        pthread_cond_signal(&server->all_handlers_done);
    pthread_mutex_unlock(&server->active_handlers_lock);
}

/* ============================================================================
 * SERVER CREATE
 * ========================================================================= */

static distric_err_t server_create_internal(
    tcp_server_t*              tcp_server,
    metrics_registry_t*        metrics,
    logger_t*                  logger,
    tracer_t*                  tracer,
    const rpc_server_config_t* config,
    rpc_server_t**             server_out)
{
    if (!tcp_server || !server_out) return DISTRIC_ERR_INVALID_ARG;

    rpc_server_t* server = (rpc_server_t*)calloc(1, sizeof(rpc_server_t));
    if (!server) return DISTRIC_ERR_NO_MEMORY;

    server->tcp_server  = tcp_server;
    server->metrics     = metrics;
    server->logger      = logger;
    server->tracer      = tracer;

    server->max_inflight_requests =
        (config && config->max_inflight_requests)
        ? config->max_inflight_requests : RPC_DEFAULT_MAX_INFLIGHT;

    server->drain_timeout_ms =
        (config && config->drain_timeout_ms)
        ? config->drain_timeout_ms : RPC_DEFAULT_DRAIN_TIMEOUT_MS;

    server->max_message_size =
        (config && config->max_message_size)
        ? config->max_message_size : RPC_MAX_MESSAGE_SIZE;

    atomic_init(&server->active_requests, 0);
    server->accepting_requests  = false;
    server->active_handlers_count = 0;

    pthread_rwlock_init(&server->handlers_lock, NULL);
    pthread_mutex_init(&server->active_handlers_lock, NULL);
    pthread_cond_init(&server->all_handlers_done, NULL);

    if (metrics) {
        metrics_register_counter(metrics, "rpc_server_requests_total",
                                 "Total RPC requests received", NULL, 0,
                                 &server->requests_total);
        metrics_register_counter(metrics, "rpc_server_errors_total",
                                 "Total RPC errors", NULL, 0,
                                 &server->errors_total);
        metrics_register_histogram(metrics, "rpc_server_request_duration_ms",
                                   "Request latency (ms)", NULL, 0,
                                   &server->latency_metric);
        metrics_register_gauge(metrics, "rpc_server_active_requests",
                               "In-flight requests", NULL, 0,
                               &server->active_requests_gauge);
        metrics_register_counter(metrics, "rpc_server_rejected_overload_total",
                                 "Requests rejected: overload", NULL, 0,
                                 &server->rejected_overload_total);
        metrics_register_counter(metrics, "rpc_server_rejected_payload_total",
                                 "Requests rejected: payload too large", NULL, 0,
                                 &server->rejected_payload_total);
    }

    *server_out = server;
    return DISTRIC_OK;
}

distric_err_t rpc_server_create(
    tcp_server_t*       tcp_server,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tracer_t*           tracer,
    rpc_server_t**      server_out)
{
    return server_create_internal(tcp_server, metrics, logger, tracer,
                                  NULL, server_out);
}

distric_err_t rpc_server_create_with_config(
    tcp_server_t*              tcp_server,
    metrics_registry_t*        metrics,
    logger_t*                  logger,
    tracer_t*                  tracer,
    const rpc_server_config_t* config,
    rpc_server_t**             server_out)
{
    return server_create_internal(tcp_server, metrics, logger, tracer,
                                  config, server_out);
}

/* ============================================================================
 * REGISTER / UNREGISTER HANDLER
 * ========================================================================= */

distric_err_t rpc_server_register_handler(
    rpc_server_t*  server,
    message_type_t msg_type,
    rpc_handler_t  handler,
    void*          userdata)
{
    if (!server || !handler) return DISTRIC_ERR_INVALID_ARG;

    pthread_rwlock_wrlock(&server->handlers_lock);
    distric_err_t err = handler_table_insert(
        server->handler_table, (uint16_t)msg_type, handler, userdata);
    pthread_rwlock_unlock(&server->handlers_lock);

    if (err == DISTRIC_OK)
        LOG_DEBUG(server->logger, "rpc_server", "Handler registered",
                  "msg_type", message_type_to_string(msg_type), NULL);
    return err;
}

/**
 * @brief Unregister handler for msg_type (Improvement #7).
 */
distric_err_t rpc_server_unregister_handler(
    rpc_server_t*  server,
    message_type_t msg_type)
{
    if (!server) return DISTRIC_ERR_INVALID_ARG;

    pthread_rwlock_wrlock(&server->handlers_lock);
    distric_err_t err = handler_table_remove(
        server->handler_table, (uint16_t)msg_type);
    pthread_rwlock_unlock(&server->handlers_lock);

    if (err == DISTRIC_OK)
        LOG_DEBUG(server->logger, "rpc_server", "Handler unregistered",
                  "msg_type", message_type_to_string(msg_type), NULL);
    return err;
}

/* ============================================================================
 * START / STOP / DESTROY
 * ========================================================================= */

distric_err_t rpc_server_start(rpc_server_t* server)
{
    if (!server) return DISTRIC_ERR_INVALID_ARG;
    server->accepting_requests = true;

    distric_err_t err = tcp_server_start(server->tcp_server,
                                          rpc_server_handle_connection, server);
    if (err != DISTRIC_OK) return err;

    char il[32], dr[32], mm[32];
    snprintf(il, sizeof(il), "%u", server->max_inflight_requests);
    snprintf(dr, sizeof(dr), "%u", server->drain_timeout_ms);
    snprintf(mm, sizeof(mm), "%u", server->max_message_size);
    LOG_INFO(server->logger, "rpc_server", "RPC server started",
             "max_inflight", il, "drain_ms", dr, "max_msg_size", mm, NULL);
    return DISTRIC_OK;
}

void rpc_server_stop(rpc_server_t* server)
{
    if (!server) return;

    /* FIX-C: set gate first, then drain, then stop transport */
    server->accepting_requests = false;

    struct timespec deadline;
    make_abs_deadline(&deadline, server->drain_timeout_ms);

    pthread_mutex_lock(&server->active_handlers_lock);
    while (server->active_handlers_count > 0) {
        int rc = pthread_cond_timedwait(&server->all_handlers_done,
                                        &server->active_handlers_lock,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            LOG_WARN(server->logger, "rpc_server",
                     "Drain timeout — forcing stop", NULL);
            break;
        }
    }
    pthread_mutex_unlock(&server->active_handlers_lock);

    tcp_server_stop(server->tcp_server);
    LOG_INFO(server->logger, "rpc_server", "RPC server stopped", NULL);
}

void rpc_server_destroy(rpc_server_t* server)
{
    if (!server) return;
    rpc_server_stop(server);
    pthread_rwlock_destroy(&server->handlers_lock);
    pthread_mutex_destroy(&server->active_handlers_lock);
    pthread_cond_destroy(&server->all_handlers_done);
    free(server);
}

/* ============================================================================
 * CLIENT CREATE
 * ========================================================================= */

static distric_err_t client_create_internal(
    tcp_pool_t*               tcp_pool,
    metrics_registry_t*       metrics,
    logger_t*                 logger,
    tracer_t*                 tracer,
    const rpc_client_config_t* config,
    rpc_client_t**            client_out)
{
    if (!tcp_pool || !client_out) return DISTRIC_ERR_INVALID_ARG;

    rpc_client_t* client = (rpc_client_t*)calloc(1, sizeof(rpc_client_t));
    if (!client) return DISTRIC_ERR_NO_MEMORY;

    client->tcp_pool = tcp_pool;
    client->metrics  = metrics;
    client->logger   = logger;
    client->tracer   = tracer;

    uint32_t max_concurrent =
        (config && config->max_concurrent_calls)
        ? config->max_concurrent_calls : RPC_DEFAULT_MAX_CONCURRENT_CALLS;

    client->pool_acquire_timeout_ms =
        (config && config->pool_acquire_timeout_ms)
        ? config->pool_acquire_timeout_ms : RPC_DEFAULT_POOL_ACQUIRE_TIMEOUT_MS;

    /* Improvement #9: store total budget */
    client->total_timeout_ms =
        (config && config->total_timeout_ms) ? config->total_timeout_ms : 0u;

    if (max_concurrent > 0) {
        client->use_sem = true;
        if (sem_init(&client->acquire_sem, 0, max_concurrent) != 0) {
            free(client);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }

    if (metrics) {
        metrics_register_counter(metrics, "rpc_client_calls_total",
                                 "Total RPC calls completed", NULL, 0,
                                 &client->calls_total);
        metrics_register_counter(metrics, "rpc_client_errors_total",
                                 "Total RPC errors", NULL, 0,
                                 &client->errors_total);
        metrics_register_histogram(metrics, "rpc_client_call_duration_ms",
                                   "Call latency (ms)", NULL, 0,
                                   &client->latency_metric);
        metrics_register_counter(metrics, "rpc_client_retries_total",
                                 "Total retried calls", NULL, 0,
                                 &client->retries_total);
        metrics_register_counter(metrics, "rpc_client_timeout_total",
                                 "Total timed-out calls", NULL, 0,
                                 &client->timeout_total);
        metrics_register_counter(metrics, "rpc_client_backpressure_total",
                                 "Calls rejected by backpressure", NULL, 0,
                                 &client->backpressure_total);
        metrics_register_counter(metrics, "rpc_client_pool_timeout_total",
                                 "Pool-acquire timeouts", NULL, 0,
                                 &client->pool_timeout_total);
        metrics_register_counter(metrics, "rpc_client_budget_exceeded_total",
                                 "Retries aborted: total budget exhausted",
                                 NULL, 0,
                                 &client->budget_exceeded_total);
    }

    *client_out = client;
    return DISTRIC_OK;
}

distric_err_t rpc_client_create(
    tcp_pool_t*         tcp_pool,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tracer_t*           tracer,
    rpc_client_t**      client_out)
{
    return client_create_internal(tcp_pool, metrics, logger, tracer,
                                  NULL, client_out);
}

distric_err_t rpc_client_create_with_config(
    tcp_pool_t*               tcp_pool,
    metrics_registry_t*       metrics,
    logger_t*                 logger,
    tracer_t*                 tracer,
    const rpc_client_config_t* config,
    rpc_client_t**            client_out)
{
    return client_create_internal(tcp_pool, metrics, logger, tracer,
                                  config, client_out);
}

void rpc_client_destroy(rpc_client_t* client)
{
    if (!client) return;
    if (client->use_sem) sem_destroy(&client->acquire_sem);
    free(client);
}

/* ============================================================================
 * rpc_call — synchronous RPC with timeout
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
    int            timeout_ms)
{
    if (!client || !host || !response_out || !resp_len_out)
        return DISTRIC_ERR_INVALID_ARG;

    if (req_len > RPC_MAX_MESSAGE_SIZE) {
        LOG_ERROR(client->logger, "rpc_client",
                  "Request exceeds RPC_MAX_MESSAGE_SIZE", NULL);
        return DISTRIC_ERR_INVALID_ARG;
    }

    *response_out = NULL;
    *resp_len_out = 0;

    uint64_t start_us = get_time_us();

    /* Fix #4: semaphore acquire */
    if (client->use_sem) {
        struct timespec abs_deadline;
        make_abs_deadline(&abs_deadline, client->pool_acquire_timeout_ms);
        int rc = sem_timedwait(&client->acquire_sem, &abs_deadline);
        if (rc != 0) {
            if (client->pool_timeout_total)
                metrics_counter_inc(client->pool_timeout_total);
            if (client->errors_total)
                metrics_counter_inc(client->errors_total);
            LOG_WARN(client->logger, "rpc_client",
                     "Pool acquire timeout — all slots busy", NULL);
            return DISTRIC_ERR_TIMEOUT;
        }
    }

    /* Acquire TCP connection */
    tcp_connection_t* conn = NULL;
    distric_err_t err = tcp_pool_acquire(client->tcp_pool, host, port, &conn);
    if (err != DISTRIC_OK) {
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return err;
    }

    /* Encode request */
    message_header_t req_hdr;
    message_header_init(&req_hdr, msg_type, (uint32_t)req_len);
    compute_header_crc32(&req_hdr, request, req_len);

    uint8_t req_hdr_buf[MESSAGE_HEADER_SIZE];
    serialize_header(&req_hdr, req_hdr_buf);

    /* Send header */
    distric_err_t send_err = tcp_send(conn, req_hdr_buf, MESSAGE_HEADER_SIZE);
    if (send_err != DISTRIC_OK) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (send_err == DISTRIC_ERR_BACKPRESSURE) {
            if (client->backpressure_total)
                metrics_counter_inc(client->backpressure_total);
        }
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return send_err;
    }

    /* Send payload */
    if (req_len > 0) {
        send_err = tcp_send(conn, request, req_len);
        if (send_err != DISTRIC_OK) {
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return send_err;
        }
    }

    /* Receive response header */
    uint8_t          resp_hdr_buf[MESSAGE_HEADER_SIZE];
    message_header_t resp_hdr;
    size_t           resp_hdr_recvd = 0;

    while (resp_hdr_recvd < MESSAGE_HEADER_SIZE) {
        int r = tcp_recv(conn,
                         resp_hdr_buf + resp_hdr_recvd,
                         MESSAGE_HEADER_SIZE - resp_hdr_recvd,
                         timeout_ms);

        distric_err_t rerr = translate_recv_error(r, timeout_ms);
        if (rerr == DISTRIC_ERR_TIMEOUT) {
            if (client->timeout_total) metrics_counter_inc(client->timeout_total);
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_TIMEOUT;
        }
        if (rerr == DISTRIC_ERR_EOF || rerr != DISTRIC_OK) {
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return rerr;
        }
        resp_hdr_recvd += (size_t)r;
    }

    if (deserialize_header(resp_hdr_buf, &resp_hdr) != DISTRIC_OK) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    if (resp_hdr.payload_len > RPC_MAX_MESSAGE_SIZE) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* Receive response payload */
    uint8_t* response = NULL;
    if (resp_hdr.payload_len > 0) {
        response = (uint8_t*)malloc(resp_hdr.payload_len);
        if (!response) {
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_NO_MEMORY;
        }

        size_t resp_recvd = 0;
        while (resp_recvd < resp_hdr.payload_len) {
            int r = tcp_recv(conn,
                             response + resp_recvd,
                             resp_hdr.payload_len - resp_recvd,
                             timeout_ms);
            distric_err_t rerr = translate_recv_error(r, timeout_ms);
            if (rerr == DISTRIC_ERR_TIMEOUT) {
                if (client->timeout_total)
                    metrics_counter_inc(client->timeout_total);
                free(response);
                tcp_pool_mark_failed(client->tcp_pool, conn);
                tcp_pool_release(client->tcp_pool, conn);
                if (client->use_sem) sem_post(&client->acquire_sem);
                if (client->errors_total) metrics_counter_inc(client->errors_total);
                return DISTRIC_ERR_TIMEOUT;
            }
            if (rerr == DISTRIC_ERR_EOF || rerr != DISTRIC_OK) {
                free(response);
                tcp_pool_mark_failed(client->tcp_pool, conn);
                tcp_pool_release(client->tcp_pool, conn);
                if (client->use_sem) sem_post(&client->acquire_sem);
                if (client->errors_total) metrics_counter_inc(client->errors_total);
                return rerr;
            }
            resp_recvd += (size_t)r;
        }
    }

    /* CRC32 verify */
    if (!verify_message_crc32(&resp_hdr, response, resp_hdr.payload_len)) {
        free(response);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    tcp_pool_release(client->tcp_pool, conn);
    if (client->use_sem) sem_post(&client->acquire_sem);

    if (client->calls_total) metrics_counter_inc(client->calls_total);
    uint64_t dur_us = get_time_us() - start_us;
    if (client->latency_metric)
        metrics_histogram_observe(client->latency_metric,
                                  (double)dur_us / 1000.0);

    char dur_str[32];
    snprintf(dur_str, sizeof(dur_str), "%llu",
             (unsigned long long)(dur_us / 1000));
    LOG_DEBUG(client->logger, "rpc_client", "RPC call completed",
              "msg_type", message_type_to_string(msg_type),
              "duration_ms", dur_str, NULL);

    *response_out = response;
    *resp_len_out = resp_hdr.payload_len;
    return DISTRIC_OK;
}

/* ============================================================================
 * rpc_call_with_retry
 *
 * Improvements #8 (jitter) and #9 (total timeout budget).
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
    int            max_retries)
{
    if (!client) return DISTRIC_ERR_INVALID_ARG;

    distric_err_t err     = DISTRIC_OK;
    int           backoff = 50; /* ms — base for jitter */

    /* Improvement #9: wall-clock budget */
    uint64_t wall_start  = get_time_us();
    uint64_t budget_us   = (client->total_timeout_ms > 0)
                           ? (uint64_t)client->total_timeout_ms * 1000ULL
                           : UINT64_MAX;

    /* Jitter seed (Improvement #8): monotonic clock XOR thread id */
    unsigned jitter_seed = (unsigned)(wall_start ^ (uintptr_t)pthread_self());

    for (int attempt = 0; attempt <= max_retries; attempt++) {

        /* --- Improvement #9: check budget before each attempt --- */
        if (budget_us != UINT64_MAX) {
            uint64_t elapsed_us = get_time_us() - wall_start;
            if (elapsed_us >= budget_us) {
                LOG_WARN(client->logger, "rpc_client",
                         "Total retry budget exhausted", NULL);
                if (client->budget_exceeded_total)
                    metrics_counter_inc(client->budget_exceeded_total);
                if (client->errors_total)
                    metrics_counter_inc(client->errors_total);
                return DISTRIC_ERR_TIMEOUT;
            }

            /* Trim per-attempt timeout to remaining budget */
            uint64_t remaining_ms = (budget_us - elapsed_us) / 1000ULL;
            if (remaining_ms < (uint64_t)BUDGET_MIN_TIMEOUT_MS) {
                /* Budget almost gone — not enough for a useful attempt */
                if (client->budget_exceeded_total)
                    metrics_counter_inc(client->budget_exceeded_total);
                return DISTRIC_ERR_TIMEOUT;
            }
            if (timeout_ms > 0 && remaining_ms < (uint64_t)timeout_ms) {
                timeout_ms = (int)remaining_ms;
            }
        }

        /* --- Backoff with jitter (Improvement #8) --- */
        if (attempt > 0) {
            if (client->retries_total) metrics_counter_inc(client->retries_total);

            /*
             * Full-jitter backoff: delay ∈ [backoff/2, backoff].
             * This desynchronises simultaneous retries from many callers,
             * preventing thundering-herd spikes after a server restart.
             */
            int half     = backoff / 2;
            int jitter   = (int)((unsigned)rand_r(&jitter_seed) %
                                 (unsigned)(half + 1));
            int delay_ms = half + jitter;   /* [backoff/2, backoff] */

            /* Cap delay so it doesn't exceed remaining budget */
            if (budget_us != UINT64_MAX) {
                uint64_t elapsed_us = get_time_us() - wall_start;
                uint64_t remaining_ms =
                    (budget_us > elapsed_us)
                    ? (budget_us - elapsed_us) / 1000ULL : 0ULL;
                if ((uint64_t)delay_ms > remaining_ms)
                    delay_ms = (int)remaining_ms;
            }

            if (delay_ms > 0) {
                struct timespec ts = {
                    .tv_sec  = delay_ms / 1000,
                    .tv_nsec = (delay_ms % 1000) * 1000000L
                };
                nanosleep(&ts, NULL);
            }

            /* Exponential growth, capped at 5 s */
            backoff = (backoff * 2 < 5000) ? backoff * 2 : 5000;
        }

        err = rpc_call(client, host, port, msg_type, request, req_len,
                       response_out, resp_len_out, timeout_ms);

        if (err == DISTRIC_OK) return DISTRIC_OK;

        rpc_error_class_t cls = rpc_error_classify(err);
        if (cls == RPC_ERR_CLASS_INVALID || cls == RPC_ERR_CLASS_INTERNAL)
            return err; /* non-retryable */
    }
    return err;
}