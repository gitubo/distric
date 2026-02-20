/**
 * @file rpc.c
 * @brief RPC Framework - Production implementation
 *
 * Fix #4 — Pool starvation prevention:
 *   A POSIX counting semaphore (sem_t) bounds concurrent outbound calls to
 *   rpc_client_config_t.max_concurrent_calls.  sem_timedwait() with
 *   pool_acquire_timeout_ms fires DISTRIC_ERR_TIMEOUT instead of blocking
 *   indefinitely when all pool connections are busy.
 *   A dedicated metric (rpc_client_pool_timeout_total) is incremented on
 *   every pool-acquire timeout so operators can alert on it.
 *
 * Fix #5 — O(1) handler dispatch:
 *   Replaced the O(n) linear scan (handlers[0..n]) with an open-addressing
 *   hash table (HANDLER_TABLE_SIZE = 64 slots, Knuth multiplicative hash).
 *   More importantly, the read-lock is released BEFORE the application
 *   handler is called.  The lock is now held only for the hash-table lookup
 *   (a few nanoseconds), not for the entire handler duration.
 *
 * Bug-fixes applied in this revision:
 *
 *  FIX-A — translate_recv_error must use DISTRIC_ERR_TIMEOUT, not raw errno:
 *    tcp_recv() returns DISTRIC_ERR_TIMEOUT (-17) on timeout, not the raw
 *    negated errno values -ETIMEDOUT (-110) or -EAGAIN (-11) that the old code
 *    checked.  None matched, so every timeout fell through to the default and
 *    was misreported as DISTRIC_ERR_IO (-14).  Callers saw -21
 *    (DISTRIC_ERR_UNAVAILABLE) because rpc_call then failed at tcp_send on a
 *    server-closed connection, silently bypassing retry logic.
 *    Fix: check (int)DISTRIC_ERR_TIMEOUT and (int)DISTRIC_ERR_EOF first;
 *    keep the raw-errno arms as belt-and-suspenders.
 *
 *  FIX-B — Server header-recv loop must recognise DISTRIC_ERR_TIMEOUT (-17):
 *    Same root cause as FIX-A.  The server loop checked -ETIMEDOUT (-110) but
 *    tcp_recv returns -17.  Every idle poll cycle hit LOG_WARN + break and
 *    closed the connection prematurely — the client's subsequent tcp_send
 *    failed with DISTRIC_ERR_UNAVAILABLE (-21).  Fix: check
 *    (int)DISTRIC_ERR_TIMEOUT (and DISTRIC_ERR_EOF for clean peer close).
 *
 *  FIX-B2 — Payload recv needs the same retry loop as header recv (Critical):
 *    The payload recv (step 5) used a single tcp_recv call treated as fatal on
 *    any non-exact return.  On non-blocking epoll transports tcp_recv returns
 *    DISTRIC_ERR_TIMEOUT (-17) when data hasn't arrived in that poll window —
 *    even for small payloads when there's a scheduler delay between the
 *    client's header-send and payload-send.  This caused "Incomplete payload
 *    received" to be logged and the connection to break before the user handler
 *    was ever called, making test_graceful_drain fail (drain_handler_done never
 *    set) and all echo/roundtrip tests return -21 (DISTRIC_ERR_UNAVAILABLE).
 *    Fix: wrap payload recv in an accumulating retry loop identical in spirit
 *    to the header recv loop — continue on DISTRIC_ERR_TIMEOUT, abort on real
 *    errors or peer close, check accepting_requests between retries.
 *
 *  FIX-C — Correct teardown order for graceful drain (Critical):
 *    rpc_server_stop() previously called tcp_server_stop() before entering
 *    the pthread_cond_timedwait drain loop.  Stopping the TCP layer kills
 *    worker threads, aborting in-flight handlers before they can signal
 *    all_handlers_done.  Order corrected: set accepting_requests=false, drain,
 *    then stop TCP.
 *
 *  FIX-D — Correct error taxonomy for network I/O (Important):
 *    DISTRIC_ERR_IO and DISTRIC_ERR_INIT_FAILED now map to
 *    RPC_ERR_CLASS_UNAVAILABLE (connection-level, retryable) instead of
 *    RPC_ERR_CLASS_INTERNAL.  This allows rpc_call_with_retry to retry on
 *    transient connection failures rather than aborting permanently.
 *
 *  FIX-E — Eliminate silent connection drops (Observability):
 *    Added LOG_ERROR statements for malloc failures and incomplete payload
 *    reads in rpc_server_handle_connection so operators can diagnose memory
 *    pressure and truncated-message attacks.
 *
 * Pre-existing improvements:
 *  - P0: Maximum payload size enforcement (RPC_MAX_MESSAGE_SIZE)
 *  - P0: Backpressure propagation
 *  - Admission control: atomic active_requests + max_inflight
 *  - Graceful drain with configurable drain_timeout_ms
 *  - Structured error taxonomy
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

#define RECV_LOOP_TIMEOUT_MS  1000   /* Poll granularity while waiting for header */

/*
 * Fix #5 — Handler dispatch hash table.
 * Size must be a power of 2.  64 slots gives load factor < 0.33 for the
 * typical ≤20 registered handlers.  msg_type == 0 is the empty-slot sentinel
 * (validate_message_header rejects msg_type == 0, so no collision).
 */
#define HANDLER_TABLE_SIZE    64
#define HANDLER_TABLE_MASK    (HANDLER_TABLE_SIZE - 1)

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

/*
 * FIX-D: DISTRIC_ERR_IO and DISTRIC_ERR_INIT_FAILED are connection-level
 * failures that are transient and retryable — they belong under UNAVAILABLE,
 * not INTERNAL.  Moving them here lets rpc_call_with_retry retry on broken
 * connections instead of giving up permanently.
 */
rpc_error_class_t rpc_error_classify(distric_err_t err)
{
    switch (err) {
        case DISTRIC_OK:               return RPC_ERR_CLASS_OK;
        case DISTRIC_ERR_TIMEOUT:      return RPC_ERR_CLASS_TIMEOUT;
        case DISTRIC_ERR_UNAVAILABLE:
        case DISTRIC_ERR_IO:           /* FIX-D: was RPC_ERR_CLASS_INTERNAL */
        case DISTRIC_ERR_INIT_FAILED:  /* FIX-D: was RPC_ERR_CLASS_INTERNAL (default) */
                                       return RPC_ERR_CLASS_UNAVAILABLE;
        case DISTRIC_ERR_BACKPRESSURE: return RPC_ERR_CLASS_BACKPRESSURE;
        case DISTRIC_ERR_INVALID_ARG:
        case DISTRIC_ERR_INVALID_FORMAT:
        case DISTRIC_ERR_TYPE_MISMATCH:return RPC_ERR_CLASS_INVALID;
        case DISTRIC_ERR_NO_MEMORY:    return RPC_ERR_CLASS_INTERNAL;
        default:                       return RPC_ERR_CLASS_INTERNAL;
    }
}

/* ============================================================================
 * HANDLER TABLE — Fix #5
 * ========================================================================= */

typedef struct {
    uint16_t      msg_type; /* 0 = empty slot */
    rpc_handler_t handler;
    void*         userdata;
} handler_slot_t;

/**
 * @brief Knuth multiplicative hash for 16-bit msg_type keys.
 *
 * Distributes the sparse message-type namespace (0x0101..0x04xx) uniformly
 * across 64 slots.
 */
static inline size_t handler_hash(uint16_t msg_type)
{
    /* Multiply by golden-ratio constant, take top 6 bits */
    return ((uint32_t)msg_type * 2654435769u) >> (32 - 6);
}

/**
 * @brief Look up a slot by msg_type (open addressing, linear probing).
 *
 * Returns a pointer to the matching slot, or NULL if not found.
 * MUST be called with handlers_lock held for reading.
 */
static const handler_slot_t* handler_table_find(
    const handler_slot_t* table, uint16_t msg_type)
{
    size_t idx = handler_hash(msg_type) & HANDLER_TABLE_MASK;

    for (size_t i = 0; i < HANDLER_TABLE_SIZE; i++) {
        const handler_slot_t* slot = &table[(idx + i) & HANDLER_TABLE_MASK];
        if (slot->msg_type == 0)        return NULL;  /* empty → not found */
        if (slot->msg_type == msg_type) return slot;
    }
    return NULL; /* table full and no match */
}

/**
 * @brief Insert into the hash table.
 *
 * Returns DISTRIC_ERR_REGISTRY_FULL if the table is too crowded.
 * MUST be called with handlers_lock held for writing.
 */
static distric_err_t handler_table_insert(
    handler_slot_t* table, uint16_t msg_type,
    rpc_handler_t handler, void* userdata)
{
    size_t idx = handler_hash(msg_type) & HANDLER_TABLE_MASK;

    for (size_t i = 0; i < HANDLER_TABLE_SIZE; i++) {
        handler_slot_t* slot = &table[(idx + i) & HANDLER_TABLE_MASK];
        if (slot->msg_type == 0 || slot->msg_type == msg_type) {
            slot->msg_type = msg_type;
            slot->handler  = handler;
            slot->userdata = userdata;
            return DISTRIC_OK;
        }
    }
    return DISTRIC_ERR_REGISTRY_FULL;
}

/* ============================================================================
 * RPC SERVER STRUCT
 * ========================================================================= */

struct rpc_server {
    tcp_server_t* tcp_server;

    /* Fix #5: O(1) hash dispatch table */
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
    metric_t* active_requests_gauge;
    metric_t* rejected_overload_total;
    metric_t* rejected_payload_total;
};

/* ============================================================================
 * RPC CLIENT STRUCT — Fix #4
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

    metric_t* calls_total;
    metric_t* errors_total;
    metric_t* latency_metric;
    metric_t* retries_total;
    metric_t* timeout_total;
    metric_t* backpressure_total;
    metric_t* pool_timeout_total;  /* Fix #4: pool-acquire timeout metric */
};

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
 * ERROR HELPERS
 * ========================================================================= */

/*
 * FIX-A: tcp_recv() returns DISTRIC_ERR_* codes directly (not raw negated
 * errno).  The previous revision checked -ETIMEDOUT (-110), -EAGAIN (-11),
 * -EWOULDBLOCK (-11) — none of which equal DISTRIC_ERR_TIMEOUT (-17).
 * Every timeout therefore fell through to the default and was misreported as
 * DISTRIC_ERR_IO, breaking the retry path and all round-trip tests.
 *
 * Fix: check DISTRIC_ERR_TIMEOUT and DISTRIC_ERR_EOF first.  Keep the raw-
 * errno arms as belt-and-suspenders for any transport that does expose them.
 */
static distric_err_t translate_recv_error(int rc, int timeout_ms)
{
    /* Primary path — transport returns DISTRIC_ERR_* directly */
    if (rc == (int)DISTRIC_ERR_TIMEOUT)  return DISTRIC_ERR_TIMEOUT; /* FIX-A */
    if (rc == (int)DISTRIC_ERR_EOF)      return DISTRIC_ERR_EOF;     /* FIX-A */

    /* Belt-and-suspenders — raw negated errno from older transport impls */
    if (rc == -ETIMEDOUT || rc == -EAGAIN || rc == -EWOULDBLOCK)
        return DISTRIC_ERR_TIMEOUT;

    /* EOF signalled as byte count 0 */
    if (rc == 0 && timeout_ms > 0)  return DISTRIC_ERR_TIMEOUT;
    if (rc == 0)                    return DISTRIC_ERR_EOF;

    return DISTRIC_ERR_IO;
}

/* ============================================================================
 * SERVER CONNECTION HANDLER
 * ========================================================================= */

static void rpc_server_handle_connection(tcp_connection_t* conn, void* userdata)
{
    rpc_server_t* server = (rpc_server_t*)userdata;

    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count++;
    pthread_mutex_unlock(&server->active_handlers_lock);

    if (!server->accepting_requests) {
        goto cleanup_and_exit;
    }

    while (server->accepting_requests) {

        uint64_t      start_time = get_time_us();
        trace_span_t* span       = NULL;

        if (server->tracer) {
            trace_start_span(server->tracer, "rpc_server_handle_request", &span);
        }

        /* ---- 1. Receive header ---- */
        uint8_t          header_buf[MESSAGE_HEADER_SIZE];
        message_header_t header;

        int received = tcp_recv(conn, header_buf, MESSAGE_HEADER_SIZE,
                                RECV_LOOP_TIMEOUT_MS);

        /* EOF signalled as 0 or DISTRIC_ERR_EOF */
        if (received == 0 || received == (int)DISTRIC_ERR_EOF) {
            if (span) { trace_set_status(span, SPAN_STATUS_OK);
                        trace_finish_span(server->tracer, span); }
            break; /* clean EOF */
        }
        if (received < 0) {
            /*
             * FIX-B: tcp_recv() returns DISTRIC_ERR_TIMEOUT (-17) on a poll
             * timeout — NOT raw errno values -ETIMEDOUT (-110), -EAGAIN (-11)
             * or -EWOULDBLOCK (-11) that the previous revision checked.
             * Because none of those matched -17, every "no data yet" poll
             * cycle fell through to LOG_WARN + break, closing the connection
             * before the client's request had been fully received.
             */
            if (received == (int)DISTRIC_ERR_TIMEOUT ||
                /* belt-and-suspenders for raw-errno transport impls */
                received == -ETIMEDOUT               ||
                received == -EAGAIN                  ||
                received == -EWOULDBLOCK) {
                if (span) trace_finish_span(server->tracer, span);
                if (!server->accepting_requests) break;
                continue;
            }
            LOG_WARN(server->logger, "rpc_server", "Header recv error", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }
        if (received != MESSAGE_HEADER_SIZE) {
            LOG_ERROR(server->logger, "rpc_server", "Incomplete header", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 2. Deserialise + validate ---- */
        deserialize_header(header_buf, &header);
        if (!validate_message_header(&header)) {
            LOG_ERROR(server->logger, "rpc_server", "Invalid message header", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 3. Payload size check ---- */
        if (header.payload_len > server->max_message_size) {
            char ls[32], ms[32];
            snprintf(ls, sizeof(ls), "%u", header.payload_len);
            snprintf(ms, sizeof(ms), "%u", server->max_message_size);
            LOG_ERROR(server->logger, "rpc_server",
                      "Payload exceeds limit — closing connection",
                      "len", ls, "max", ms, NULL);
            if (server->rejected_payload_total)
                metrics_counter_inc(server->rejected_payload_total);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 4. Admission control ---- */
        uint32_t current = (uint32_t)atomic_fetch_add_explicit(
                               &server->active_requests, 1,
                               memory_order_acquire);
        if (current >= server->max_inflight_requests) {
            atomic_fetch_sub_explicit(&server->active_requests, 1,
                                      memory_order_release);
            LOG_WARN(server->logger, "rpc_server", "Max inflight exceeded", NULL);
            if (server->rejected_overload_total)
                metrics_counter_inc(server->rejected_overload_total);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break; /* close connection; client will retry */
        }

        if (server->requests_total) metrics_counter_inc(server->requests_total);

        /* ---- 5. Receive payload ---- */
        /*
         * FIX-B2: Mirror the header-recv retry logic for the payload.
         *
         * tcp_recv() may return DISTRIC_ERR_TIMEOUT (-17) when the transport's
         * epoll fires but the payload bytes haven't arrived yet (e.g. large
         * payloads spanning multiple kernel buffers, or a scheduler delay
         * between the client's header-send and payload-send).  The original
         * code treated any non-exact count as a fatal "Incomplete payload" and
         * broke the connection, causing the user handler to never be invoked
         * and silently failing the graceful-drain test.
         *
         * Fix: use the same accumulating retry loop already applied to the
         * header recv — continue on DISTRIC_ERR_TIMEOUT, abort on real errors
         * or peer close.  Also bail if accepting_requests becomes false mid-
         * receive so the server can stop cleanly without holding stale data.
         */
        uint8_t* payload = NULL;
        if (header.payload_len > 0) {
            payload = (uint8_t*)malloc(header.payload_len);
            if (!payload) {
                LOG_ERROR(server->logger, "rpc_server",
                          "Out of memory allocating payload", NULL);
                atomic_fetch_sub_explicit(&server->active_requests, 1,
                                          memory_order_release);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }

            size_t bytes_recvd = 0;
            bool   payload_ok  = false;
            while (bytes_recvd < header.payload_len) {
                received = tcp_recv(conn,
                                    payload + bytes_recvd,
                                    header.payload_len - bytes_recvd,
                                    RECV_LOOP_TIMEOUT_MS);

                if (received == (int)DISTRIC_ERR_TIMEOUT ||
                    received == -ETIMEDOUT               ||
                    received == -EAGAIN                  ||
                    received == -EWOULDBLOCK) {
                    /* Transient: no bytes this window; keep waiting.
                     * FIX-B2: this is the critical path that was missing. */
                    if (!server->accepting_requests) break; /* server stopping */
                    continue;
                }
                if (received == 0 || received == (int)DISTRIC_ERR_EOF) {
                    break; /* peer closed cleanly mid-payload */
                }
                if (received < 0) {
                    break; /* real transport error */
                }
                bytes_recvd += (size_t)received;
            }

            if (bytes_recvd != header.payload_len) {
                if (bytes_recvd > 0) {
                    LOG_ERROR(server->logger, "rpc_server",
                              "Incomplete payload received", NULL);
                }
                free(payload);
                atomic_fetch_sub_explicit(&server->active_requests, 1,
                                          memory_order_release);
                if (server->errors_total) metrics_counter_inc(server->errors_total);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
            payload_ok = true;
            (void)payload_ok; /* used implicitly: bytes_recvd == header.payload_len */
        }

        /* ---- 6. CRC32 ---- */
        if (!verify_message_crc32(&header, payload, header.payload_len)) {
            free(payload);
            atomic_fetch_sub_explicit(&server->active_requests, 1,
                                      memory_order_release);
            LOG_ERROR(server->logger, "rpc_server", "CRC32 mismatch", NULL);
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            if (span) trace_finish_span(server->tracer, span);
            break;
        }

        /* ---- 7. Dispatch — Fix #5 ----------------------------------------
         *
         * (a) Acquire read lock.
         * (b) Look up handler in O(1) hash table.
         * (c) Copy handler + userdata to locals.
         * (d) Release read lock BEFORE calling handler.
         *     → slow handlers no longer starve registration.
         * ------------------------------------------------------------------ */
        pthread_rwlock_rdlock(&server->handlers_lock);
        const handler_slot_t* slot =
            handler_table_find(server->handler_table,
                               (uint16_t)header.msg_type);

        rpc_handler_t handler_fn  = slot ? slot->handler  : NULL;
        void*         handler_ud  = slot ? slot->userdata : NULL;
        pthread_rwlock_unlock(&server->handlers_lock);   /* lock released here */

        if (!handler_fn) {
            atomic_fetch_sub_explicit(&server->active_requests, 1,
                                      memory_order_release);
            if (server->accepting_requests) {
                LOG_WARN(server->logger, "rpc_server", "No handler registered",
                         "msg_type", message_type_to_string(
                             (message_type_t)header.msg_type), NULL);
            }
            if (server->errors_total) metrics_counter_inc(server->errors_total);
            free(payload);
            if (span) trace_finish_span(server->tracer, span);
            continue; /* unknown type — not fatal */
        }

        /* ---- 8. Call handler (lock NOT held) ---- */
        uint8_t* response  = NULL;
        size_t   resp_len  = 0;
        int handler_result = handler_fn(payload, header.payload_len,
                                        &response, &resp_len,
                                        handler_ud, span);
        free(payload);

        atomic_fetch_sub_explicit(&server->active_requests, 1,
                                  memory_order_release);

        /* ---- 9. Send response ---- */
        if (handler_result == 0) {
            message_header_t resp_hdr;
            message_header_init(&resp_hdr, (message_type_t)header.msg_type,
                                (uint32_t)resp_len);
            compute_header_crc32(&resp_hdr, response, resp_len);

            uint8_t resp_hdr_buf[MESSAGE_HEADER_SIZE];
            serialize_header(&resp_hdr, resp_hdr_buf);

            distric_err_t send_err = tcp_send(conn, resp_hdr_buf,
                                              MESSAGE_HEADER_SIZE);
            if (send_err == DISTRIC_ERR_BACKPRESSURE) {
                LOG_WARN(server->logger, "rpc_server",
                         "Backpressure on response send — closing", NULL);
                free(response);
                if (span) trace_finish_span(server->tracer, span);
                break;
            }
            if (send_err == DISTRIC_OK && resp_len > 0) {
                tcp_send(conn, response, resp_len);
            }
        }
        free(response);

        /* ---- 10. Latency metric ---- */
        uint64_t dur_us = get_time_us() - start_time;
        if (server->latency_metric) {
            metrics_histogram_observe(server->latency_metric,
                                      (double)dur_us / 1000.0);
        }

        if (span) {
            trace_set_status(span, SPAN_STATUS_OK);
            trace_finish_span(server->tracer, span);
        }
    }

cleanup_and_exit:
    pthread_mutex_lock(&server->active_handlers_lock);
    server->active_handlers_count--;
    if (server->active_handlers_count == 0) {
        pthread_cond_broadcast(&server->all_handlers_done);
    }
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

    /* Apply config */
    uint32_t max_inflight  = RPC_DEFAULT_MAX_INFLIGHT;
    uint32_t drain_ms      = RPC_DEFAULT_DRAIN_TIMEOUT_MS;
    uint32_t max_msg_size  = RPC_MAX_MESSAGE_SIZE;

    if (config) {
        if (config->max_inflight_requests) max_inflight = config->max_inflight_requests;
        if (config->drain_timeout_ms)      drain_ms     = config->drain_timeout_ms;
        if (config->max_message_size)      max_msg_size = config->max_message_size;
    }

    server->max_inflight_requests = max_inflight;
    server->drain_timeout_ms      = drain_ms;
    server->max_message_size      = max_msg_size;
    server->accepting_requests    = false;

    /* Handler table — empty (msg_type == 0 marks free slots) */
    memset(server->handler_table, 0, sizeof(server->handler_table));

    atomic_init(&server->active_requests, 0);
    pthread_rwlock_init(&server->handlers_lock, NULL);
    pthread_mutex_init(&server->active_handlers_lock, NULL);
    pthread_cond_init(&server->all_handlers_done, NULL);

    if (metrics) {
        metrics_register_counter(metrics, "rpc_server_requests_total",
                                 "Total requests processed", NULL, 0,
                                 &server->requests_total);
        metrics_register_counter(metrics, "rpc_server_errors_total",
                                 "Total server errors", NULL, 0,
                                 &server->errors_total);
        metrics_register_histogram(metrics, "rpc_server_latency_ms",
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
 * REGISTER HANDLER — Fix #5: O(1) insert
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

    if (err == DISTRIC_OK) {
        LOG_DEBUG(server->logger, "rpc_server", "Handler registered",
                  "msg_type", message_type_to_string(msg_type), NULL);
    }
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

/*
 * FIX-C: Correct teardown order for graceful drain.
 *
 * Previous order:
 *   1. accepting_requests = false
 *   2. tcp_server_stop()          ← kills worker threads immediately
 *   3. pthread_cond_timedwait()   ← workers already dead; never signals
 *
 * Fixed order:
 *   1. accepting_requests = false  (gate: no new request processing)
 *   2. pthread_cond_timedwait()    (wait for in-flight handlers to finish)
 *   3. tcp_server_stop()           (safe to kill transport now)
 *
 * This allows active handlers to complete their work and send responses
 * before the underlying TCP connections are forcibly closed.
 */
void rpc_server_stop(rpc_server_t* server)
{
    if (!server) return;

    LOG_INFO(server->logger, "rpc_server", "RPC server stopping", NULL);

    /* Step 1: prevent new request dispatch */
    server->accepting_requests = false;

    /* Step 2: wait for in-flight handlers to drain */
    pthread_mutex_lock(&server->active_handlers_lock);
    struct timespec deadline;
    make_abs_deadline(&deadline, server->drain_timeout_ms);

    while (server->active_handlers_count > 0) {
        int rc = pthread_cond_timedwait(&server->all_handlers_done,
                                        &server->active_handlers_lock,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%zu", server->active_handlers_count);
            LOG_WARN(server->logger, "rpc_server",
                     "Drain timeout: handlers still active", "count", cnt, NULL);
            break;
        }
    }
    pthread_mutex_unlock(&server->active_handlers_lock);

    /* Step 3: now safe to stop the transport layer */
    tcp_server_stop(server->tcp_server);

    LOG_INFO(server->logger, "rpc_server", "RPC server stopped", NULL);
}

void rpc_server_destroy(rpc_server_t* server)
{
    if (!server) return;
    pthread_rwlock_destroy(&server->handlers_lock);
    pthread_mutex_destroy(&server->active_handlers_lock);
    pthread_cond_destroy(&server->all_handlers_done);
    free(server);
}

/* ============================================================================
 * CLIENT CREATE — Fix #4: Pool starvation prevention
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

    uint32_t max_concurrent       = RPC_DEFAULT_MAX_CONCURRENT_CALLS;
    uint32_t pool_acquire_timeout = RPC_DEFAULT_POOL_ACQUIRE_TIMEOUT_MS;

    if (config) {
        if (config->max_concurrent_calls)    max_concurrent       = config->max_concurrent_calls;
        if (config->pool_acquire_timeout_ms) pool_acquire_timeout = config->pool_acquire_timeout_ms;
    }

    client->pool_acquire_timeout_ms = pool_acquire_timeout;

    if (max_concurrent > 0) {
        client->use_sem = true;
        sem_init(&client->acquire_sem, 0, max_concurrent);
    } else {
        client->use_sem = false;
    }

    if (metrics) {
        metrics_register_counter(metrics, "rpc_client_calls_total",
                                 "Total RPC calls made", NULL, 0,
                                 &client->calls_total);
        metrics_register_counter(metrics, "rpc_client_errors_total",
                                 "Total RPC client errors", NULL, 0,
                                 &client->errors_total);
        metrics_register_histogram(metrics, "rpc_client_call_duration_ms",
                                   "RPC call duration (ms)", NULL, 0,
                                   &client->latency_metric);
        metrics_register_counter(metrics, "rpc_client_retries_total",
                                 "Total retry attempts", NULL, 0,
                                 &client->retries_total);
        metrics_register_counter(metrics, "rpc_client_timeout_total",
                                 "Total timeout errors", NULL, 0,
                                 &client->timeout_total);
        metrics_register_counter(metrics, "rpc_client_backpressure_total",
                                 "Total backpressure events", NULL, 0,
                                 &client->backpressure_total);
        metrics_register_counter(metrics, "rpc_client_pool_timeout_total",
                                 "Total pool-acquire timeouts", NULL, 0,
                                 &client->pool_timeout_total);
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

    /* ---- Fix #4: semaphore acquire ---- */
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

    /* ---- Acquire TCP connection ---- */
    tcp_connection_t* conn = NULL;
    distric_err_t err = tcp_pool_acquire(client->tcp_pool, host, port, &conn);
    if (err != DISTRIC_OK) {
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return err;
    }

    /* ---- Build + send request header ---- */
    message_header_t req_hdr;
    message_header_init(&req_hdr, msg_type, (uint32_t)req_len);
    compute_header_crc32(&req_hdr, request, req_len);

    uint8_t req_hdr_buf[MESSAGE_HEADER_SIZE];
    serialize_header(&req_hdr, req_hdr_buf);

    err = tcp_send(conn, req_hdr_buf, MESSAGE_HEADER_SIZE);
    if (err == DISTRIC_ERR_BACKPRESSURE) {
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->backpressure_total) metrics_counter_inc(client->backpressure_total);
        if (client->errors_total)       metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_BACKPRESSURE;
    }
    if (err != DISTRIC_OK) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_UNAVAILABLE;
    }

    if (req_len > 0) {
        err = tcp_send(conn, request, req_len);
        if (err == DISTRIC_ERR_BACKPRESSURE) {
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->backpressure_total) metrics_counter_inc(client->backpressure_total);
            if (client->errors_total)       metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_BACKPRESSURE;
        }
        if (err != DISTRIC_OK) {
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_UNAVAILABLE;
        }
    }

    /* ---- Receive response header ---- */
    uint8_t resp_hdr_buf[MESSAGE_HEADER_SIZE];
    int received = tcp_recv(conn, resp_hdr_buf, MESSAGE_HEADER_SIZE, timeout_ms);

    if (received != MESSAGE_HEADER_SIZE) {
        distric_err_t rerr = translate_recv_error(received, timeout_ms);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (rerr == DISTRIC_ERR_TIMEOUT && client->timeout_total)
            metrics_counter_inc(client->timeout_total);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return rerr;
    }

    message_header_t resp_hdr;
    deserialize_header(resp_hdr_buf, &resp_hdr);

    if (!validate_message_header(&resp_hdr)) {
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    if (resp_hdr.payload_len > RPC_MAX_MESSAGE_SIZE) {
        LOG_ERROR(client->logger, "rpc_client",
                  "Response payload exceeds RPC_MAX_MESSAGE_SIZE", NULL);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* ---- Receive response payload ---- */
    uint8_t* response = NULL;
    if (resp_hdr.payload_len > 0) {
        response = (uint8_t*)malloc(resp_hdr.payload_len);
        if (!response) {
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return DISTRIC_ERR_NO_MEMORY;
        }
        received = tcp_recv(conn, response, resp_hdr.payload_len, timeout_ms);
        if (received != (int)resp_hdr.payload_len) {
            free(response);
            distric_err_t rerr = translate_recv_error(received, timeout_ms);
            tcp_pool_mark_failed(client->tcp_pool, conn);
            tcp_pool_release(client->tcp_pool, conn);
            if (client->use_sem) sem_post(&client->acquire_sem);
            if (rerr == DISTRIC_ERR_TIMEOUT && client->timeout_total)
                metrics_counter_inc(client->timeout_total);
            if (client->errors_total) metrics_counter_inc(client->errors_total);
            return rerr;
        }
    }

    /* ---- CRC32 verify ---- */
    if (!verify_message_crc32(&resp_hdr, response, resp_hdr.payload_len)) {
        free(response);
        tcp_pool_mark_failed(client->tcp_pool, conn);
        tcp_pool_release(client->tcp_pool, conn);
        if (client->use_sem) sem_post(&client->acquire_sem);
        if (client->errors_total) metrics_counter_inc(client->errors_total);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* ---- Release resources ---- */
    tcp_pool_release(client->tcp_pool, conn);
    if (client->use_sem) sem_post(&client->acquire_sem);

    /* ---- Record metrics ---- */
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
 * rpc_call_with_retry — exponential backoff wrapper
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
    distric_err_t err      = DISTRIC_OK;
    int           backoff  = 50; /* ms */

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            if (client->retries_total) metrics_counter_inc(client->retries_total);
            struct timespec ts = {
                .tv_sec  = backoff / 1000,
                .tv_nsec = (backoff % 1000) * 1000000L
            };
            nanosleep(&ts, NULL);
            backoff = (backoff * 2 < 5000) ? backoff * 2 : 5000;
        }

        err = rpc_call(client, host, port, msg_type, request, req_len,
                       response_out, resp_len_out, timeout_ms);

        if (err == DISTRIC_OK) return DISTRIC_OK;

        rpc_error_class_t cls = rpc_error_classify(err);
        if (cls == RPC_ERR_CLASS_INVALID || cls == RPC_ERR_CLASS_INTERNAL) {
            return err; /* non-retryable */
        }
    }
    return err;
}