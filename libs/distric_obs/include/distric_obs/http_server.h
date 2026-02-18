/**
 * @file http_server.h
 * @brief DistriC Observability — HTTP Server Internal Details
 *
 * NOT part of the public API.  Only http_server.c may include this header.
 *
 * =============================================================================
 * PRODUCTION HARDENING APPLIED
 * =============================================================================
 *
 * #6 HTTP Server Hardening Against Slow Clients
 *   - SO_RCVTIMEO and SO_SNDTIMEO are applied to EVERY accepted socket, not
 *     just the listening socket.  This prevents slow-read clients from blocking
 *     the request handler indefinitely.
 *   - Strict max request line enforcement: any request > HTTP_MAX_REQUEST_BYTES
 *     is rejected with 400 immediately.
 *   - Response body is hard-capped at max_response_bytes before any write().
 *   - Accept loop uses a short SO_RCVTIMEO on the listening fd to enable
 *     clean shutdown without hanging in accept().
 *
 * #9 Observability Self-Monitoring Completeness
 *   - http_server_internal_metrics_t tracks per-endpoint and error counters.
 *   - distric_internal_http_requests_total (by path)
 *   - distric_internal_http_errors_4xx_total
 *   - distric_internal_http_errors_5xx_total
 *   - distric_internal_http_active_connections (gauge, approximate)
 *
 * Security model:
 *   - Read timeout enforced via SO_RCVTIMEO on EACH accepted socket.
 *   - Write timeout enforced via SO_SNDTIMEO on EACH accepted socket.
 *   - Max request line length: HTTP_MAX_REQUEST_BYTES.
 *   - Response data is ALWAYS copied from registry before connection handling.
 *     The server holds no registry locks while writing to the network.
 *   - Only GET requests accepted; others receive 405.
 *   - Path traversal patterns rejected with 400.
 *   - Only recognised paths return data; all others receive 404.
 */

#ifndef DISTRIC_HTTP_SERVER_INTERNAL_H
#define DISTRIC_HTTP_SERVER_INTERNAL_H

#include "distric_obs.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>

/* ============================================================================
 * Internal defaults
 * ========================================================================= */

#define HTTP_DEFAULT_READ_TIMEOUT_MS    5000u
#define HTTP_DEFAULT_WRITE_TIMEOUT_MS   10000u
#define HTTP_DEFAULT_MAX_RESPONSE_BYTES (4u * 1024u * 1024u)   /* 4 MiB */
#define HTTP_MAX_REQUEST_BYTES          4096u
#define HTTP_MAX_PATH_LEN               256u
#define HTTP_MAX_METHOD_LEN             8u
#define HTTP_ACCEPT_BACKLOG             32

/*
 * Accept loop SO_RCVTIMEO for the listening fd.
 * Allows the accept thread to wake up periodically and check for shutdown.
 * Using 1 second provides clean shutdown within 1s of destroy() call.
 */
#define HTTP_ACCEPT_POLL_TIMEOUT_MS     1000u

/* ============================================================================
 * Internal types
 * ========================================================================= */

typedef struct {
    char method[HTTP_MAX_METHOD_LEN];
    char path[HTTP_MAX_PATH_LEN];
} http_request_t;

/*
 * http_server_internal_metrics_t: Prometheus handles for self-monitoring.
 * Registered via http_server_register_internal_metrics().
 * All counters use distric_internal_http_* prefix (Item #9).
 */
typedef struct {
    metric_t* requests_total;          /* counter: total requests handled               */
    metric_t* errors_4xx_total;        /* counter: client-side errors (400, 404, 405)   */
    metric_t* errors_5xx_total;        /* counter: server-side errors (500, 503)        */
    metric_t* active_connections;      /* gauge:   approximate concurrent connections    */
} http_server_internal_metrics_t;

struct obs_server_s {
    int                 listen_fd;
    uint16_t            port;
    metrics_registry_t* metrics;
    health_registry_t*  health;

    uint32_t            read_timeout_ms;
    uint32_t            write_timeout_ms;
    size_t              max_response_bytes;

    pthread_t           accept_thread;
    bool                thread_started;

    /*
     * shutdown: store(release) by destroy(); load(acquire) by accept thread.
     * Ensures all in-flight requests complete before the thread exits.
     */
    _Atomic bool        shutdown;

    /*
     * active_connections: incremented before handling each connection,
     * decremented after.  Updated with relaxed ordering (approximate count).
     */
    _Atomic int32_t     active_connections;

    /* Self-monitoring handles */
    http_server_internal_metrics_t internal_metrics;
    bool                            metrics_registered;
};

#endif /* DISTRIC_HTTP_SERVER_INTERNAL_H */