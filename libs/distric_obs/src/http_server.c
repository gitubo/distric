/*
 * http_server.c — DistriC Observability Library — HTTP Server
 *
 * =============================================================================
 * PRODUCTION HARDENING APPLIED
 * =============================================================================
 *
 * #6 HTTP Server Hardening Against Slow Clients
 *   - SO_RCVTIMEO and SO_SNDTIMEO applied to EVERY accepted fd.
 *   - Strict request body size check (no body allowed; max request line 4 KiB).
 *   - Max response body cap before any network write.
 *
 * #9 Observability Self-Monitoring Completeness
 *   - distric_internal_http_requests_total
 *   - distric_internal_http_errors_4xx_total
 *   - distric_internal_http_errors_5xx_total
 *   - distric_internal_http_active_connections
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include "distric_obs/http_server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

/* ============================================================================
 * Socket helpers
 * ========================================================================= */

/*
 * apply_socket_timeouts: enforces per-connection read/write deadlines.
 *
 * IMPORTANT (Item #6): This must be called on EACH accepted socket, not
 * on the listening socket.  The listening socket has its own shorter
 * poll timeout (HTTP_ACCEPT_POLL_TIMEOUT_MS) set at bind time to allow
 * the accept loop to wake and check for shutdown.
 *
 * A slow-reading or slow-writing client will be forcibly disconnected
 * after read_ms / write_ms respectively.  This prevents a single slow
 * client from blocking the accept thread for an unbounded time.
 */
static void apply_socket_timeouts(int fd, uint32_t read_ms, uint32_t write_ms) {
    struct timeval rtv = {
        .tv_sec  = (time_t)(read_ms / 1000),
        .tv_usec = (suseconds_t)((read_ms  % 1000) * 1000)
    };
    struct timeval wtv = {
        .tv_sec  = (time_t)(write_ms / 1000),
        .tv_usec = (suseconds_t)((write_ms % 1000) * 1000)
    };
    /* Errors are intentionally ignored — timeouts are best-effort safety valve */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv));
}

/* ============================================================================
 * Request parsing
 * ========================================================================= */

/*
 * parse_request: reads the HTTP request line from client_fd.
 * Returns 0 on success, -1 on parse error, timeout, or oversized request.
 *
 * Security (Item #6):
 *   - Reads at most HTTP_MAX_REQUEST_BYTES bytes; rejects if request line
 *     is not found within that budget.
 *   - Method limited to HTTP_MAX_METHOD_LEN - 1 chars.
 *   - Path limited to HTTP_MAX_PATH_LEN - 1 chars.
 *   - Only GET method is accepted (enforced by caller for clean 405 response).
 *   - Path traversal patterns (../) are rejected with an immediate error.
 */
static int parse_request(int client_fd, http_request_t* req) {
    char buf[HTTP_MAX_REQUEST_BYTES + 1];
    size_t total   = 0;
    int    found   = 0;

    while (total < HTTP_MAX_REQUEST_BYTES) {
        ssize_t n = recv(client_fd,
                         buf + total,
                         HTTP_MAX_REQUEST_BYTES - total,
                         0);
        if (n <= 0) return -1;  /* error or timeout */
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n")) { found = 1; break; }
    }
    if (!found) return -1;

    /* Parse: METHOD SP PATH (ignore HTTP version) */
    char method[HTTP_MAX_METHOD_LEN];
    char path[HTTP_MAX_PATH_LEN];

    if (sscanf(buf, "%7s %255s", method, path) != 2) return -1;

    /* Reject path traversal */
    if (strstr(path, "..") != NULL) return -1;

    strncpy(req->method, method, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';
    strncpy(req->path, path, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    return 0;
}

/* ============================================================================
 * Response formatting and sending
 * ========================================================================= */

static void send_response_str(int fd, int code, const char* code_str,
                               const char* content_type,
                               const char* body, size_t body_len,
                               size_t max_body) {
    /* Hard cap response body (Item #6) */
    if (body_len > max_body) body_len = max_body;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "Server: DistriC-Obs/" DISTRIC_OBS_VERSION_STR "\r\n"
                        "Cache-Control: no-cache\r\n"
                        "\r\n",
                        code, code_str, content_type, body_len);

    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) return;

    /* Write header — SO_SNDTIMEO on the fd provides the deadline */
    ssize_t sent = 0;
    while (sent < hlen) {
        ssize_t n = write(fd, header + sent, (size_t)(hlen - sent));
        if (n <= 0) return;
        sent += n;
    }

    if (body && body_len > 0) {
        sent = 0;
        while (sent < (ssize_t)body_len) {
            ssize_t n = write(fd, body + sent, body_len - (size_t)sent);
            if (n <= 0) return;
            sent += n;
        }
    }
}

#define SEND_ERROR(fd, code, msg, max) \
    send_response_str((fd), (code), (msg), "text/plain", \
                      (msg), strlen(msg), (max))

/* ============================================================================
 * Self-monitoring helpers (Item #9)
 * ========================================================================= */

static void track_request(obs_server_t* server) {
    if (!server->metrics_registered) return;
    if (server->internal_metrics.requests_total)
        metrics_counter_inc(server->internal_metrics.requests_total);
}

static void track_error_4xx(obs_server_t* server) {
    if (!server->metrics_registered) return;
    if (server->internal_metrics.errors_4xx_total)
        metrics_counter_inc(server->internal_metrics.errors_4xx_total);
}

static void track_error_5xx(obs_server_t* server) {
    if (!server->metrics_registered) return;
    if (server->internal_metrics.errors_5xx_total)
        metrics_counter_inc(server->internal_metrics.errors_5xx_total);
}

/* ============================================================================
 * Request handlers
 * ========================================================================= */

static void handle_metrics(obs_server_t* server, int client_fd) {
    char*  body  = NULL;
    size_t bsize = 0;

    /*
     * Export data is COPIED into a malloc'd buffer before we touch the network.
     * The registry is accessed here; NO lock is held across any write().
     */
    distric_err_t err = metrics_export_prometheus(server->metrics, &body, &bsize);

    track_request(server);

    if (err == DISTRIC_OK && body) {
        send_response_str(client_fd, 200, "OK",
                          "text/plain; version=0.0.4; charset=utf-8",
                          body, bsize, server->max_response_bytes);
        free(body);
    } else {
        track_error_5xx(server);
        SEND_ERROR(client_fd, 500, "Internal Server Error",
                   server->max_response_bytes);
    }
}

static void handle_health_ready(obs_server_t* server, int client_fd) {
    char*  body  = NULL;
    size_t bsize = 0;

    distric_err_t err = health_export_json(server->health, &body, &bsize);

    track_request(server);

    if (err != DISTRIC_OK || !body) {
        track_error_5xx(server);
        SEND_ERROR(client_fd, 500, "Internal Server Error",
                   server->max_response_bytes);
        return;
    }

    health_status_t overall = health_get_overall_status(server->health);
    int          code = (overall == HEALTH_UP) ? 200 : 503;
    const char*  txt  = (code == 200)          ? "OK" : "Service Unavailable";

    if (code == 503) track_error_5xx(server);

    send_response_str(client_fd, code, txt,
                      "application/json",
                      body, bsize, server->max_response_bytes);
    free(body);
}

static void handle_health_live(obs_server_t* server, int client_fd) {
    track_request(server);
    const char* body = "{\"status\":\"UP\"}";
    send_response_str(client_fd, 200, "OK",
                      "application/json",
                      body, strlen(body), server->max_response_bytes);
}

static void handle_not_found(obs_server_t* server, int client_fd) {
    track_request(server);
    track_error_4xx(server);
    SEND_ERROR(client_fd, 404, "Not Found", server->max_response_bytes);
}

static void handle_method_not_allowed(obs_server_t* server, int client_fd) {
    track_request(server);
    track_error_4xx(server);
    SEND_ERROR(client_fd, 405, "Method Not Allowed", server->max_response_bytes);
}

static void handle_bad_request(obs_server_t* server, int client_fd) {
    track_request(server);
    track_error_4xx(server);
    SEND_ERROR(client_fd, 400, "Bad Request", server->max_response_bytes);
}

/* ============================================================================
 * Connection handler
 * ========================================================================= */

static void handle_connection(obs_server_t* server, int client_fd) {
    /*
     * Apply per-connection timeouts (Item #6).
     * Must be done before any recv() or send() on this fd.
     * These enforce a hard deadline regardless of client behaviour.
     */
    apply_socket_timeouts(client_fd, server->read_timeout_ms,
                          server->write_timeout_ms);

    http_request_t req;
    memset(&req, 0, sizeof(req));

    if (parse_request(client_fd, &req) != 0) {
        handle_bad_request(server, client_fd);
        return;
    }

    /* Enforce GET-only */
    if (strncmp(req.method, "GET", 3) != 0) {
        handle_method_not_allowed(server, client_fd);
        return;
    }

    if (strcmp(req.path, "/metrics") == 0) {
        handle_metrics(server, client_fd);
    } else if (strcmp(req.path, "/health/ready") == 0) {
        handle_health_ready(server, client_fd);
    } else if (strcmp(req.path, "/health/live") == 0) {
        handle_health_live(server, client_fd);
    } else {
        handle_not_found(server, client_fd);
    }
}

/* ============================================================================
 * Accept loop
 * ========================================================================= */

static void* accept_thread_fn(void* arg) {
    obs_server_t* server = (obs_server_t*)arg;

    while (!atomic_load_explicit(&server->shutdown, memory_order_acquire)) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = accept(server->listen_fd,
                                (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;   /* accept timeout — re-check shutdown */
            if (atomic_load_explicit(&server->shutdown, memory_order_acquire))
                break;
            /* Transient accept error: brief backoff and retry */
            struct timespec ts = { 0, 10000000L }; /* 10ms */
            nanosleep(&ts, NULL);
            continue;
        }

        /* Track active connections (Item #9) */
        int32_t prev =
            atomic_fetch_add_explicit(&server->active_connections, 1,
                                      memory_order_relaxed);
        if (server->metrics_registered &&
            server->internal_metrics.active_connections) {
            metrics_gauge_set(server->internal_metrics.active_connections,
                              (double)(prev + 1));
        }

        handle_connection(server, client_fd);
        close(client_fd);

        /* Decrement active connections */
        prev = atomic_fetch_sub_explicit(&server->active_connections, 1,
                                          memory_order_relaxed);
        if (server->metrics_registered &&
            server->internal_metrics.active_connections) {
            metrics_gauge_set(server->internal_metrics.active_connections,
                              (double)(prev - 1 < 0 ? 0 : prev - 1));
        }
    }

    return NULL;
}

/* ============================================================================
 * Init / Destroy
 * ========================================================================= */

distric_err_t obs_server_init(obs_server_t** out, uint16_t port,
                               metrics_registry_t* metrics,
                               health_registry_t* health) {
    obs_server_config_t cfg = {
        .port    = port,
        .metrics = metrics,
        .health  = health,
    };
    return obs_server_init_with_config(out, &cfg);
}

distric_err_t obs_server_init_with_config(obs_server_t** out,
                                           const obs_server_config_t* config) {
    if (!out || !config || !config->metrics || !config->health)
        return DISTRIC_ERR_INVALID_ARG;

    obs_server_t* server = calloc(1, sizeof(*server));
    if (!server) return DISTRIC_ERR_ALLOC_FAILURE;

    server->metrics = config->metrics;
    server->health  = config->health;
    server->read_timeout_ms  = config->read_timeout_ms
                               ? config->read_timeout_ms
                               : HTTP_DEFAULT_READ_TIMEOUT_MS;
    server->write_timeout_ms = config->write_timeout_ms
                               ? config->write_timeout_ms
                               : HTTP_DEFAULT_WRITE_TIMEOUT_MS;
    server->max_response_bytes = config->max_response_bytes
                                 ? config->max_response_bytes
                                 : HTTP_DEFAULT_MAX_RESPONSE_BYTES;

    atomic_init(&server->shutdown,            false);
    atomic_init(&server->active_connections,  0);
    server->metrics_registered = false;

    /* Create and configure listening socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { free(server); return DISTRIC_ERR_INIT_FAILED; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /*
     * Apply a short timeout on the LISTENING fd only.
     * This allows the accept loop to periodically wake and check shutdown.
     * Per-connection timeouts are applied in handle_connection() via
     * apply_socket_timeouts() BEFORE any data is read or written.
     */
    struct timeval atv = {
        .tv_sec  = (time_t)(HTTP_ACCEPT_POLL_TIMEOUT_MS / 1000),
        .tv_usec = (suseconds_t)((HTTP_ACCEPT_POLL_TIMEOUT_MS % 1000) * 1000)
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &atv, sizeof(atv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(config->port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        free(server);
        return DISTRIC_ERR_INIT_FAILED;
    }

    if (listen(fd, HTTP_ACCEPT_BACKLOG) != 0) {
        close(fd);
        free(server);
        return DISTRIC_ERR_INIT_FAILED;
    }

    /* Discover actual bound port */
    struct sockaddr_in actual;
    socklen_t actual_len = sizeof(actual);
    if (getsockname(fd, (struct sockaddr*)&actual, &actual_len) == 0)
        server->port = ntohs(actual.sin_port);
    else
        server->port = config->port;

    server->listen_fd = fd;

    if (pthread_create(&server->accept_thread, NULL,
                       accept_thread_fn, server) != 0) {
        close(fd);
        free(server);
        return DISTRIC_ERR_INIT_FAILED;
    }
    server->thread_started = true;

    *out = server;
    return DISTRIC_OK;
}

void obs_server_destroy(obs_server_t* server) {
    if (!server) return;
    /*
     * release: ensures all in-flight handling sees shutdown = true
     * after the accept thread's acquire-load.
     */
    atomic_store_explicit(&server->shutdown, true, memory_order_release);
    /* Close listen fd to unblock accept() immediately */
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    if (server->thread_started)
        pthread_join(server->accept_thread, NULL);
    free(server);
}

uint16_t obs_server_get_port(obs_server_t* server) {
    return server ? server->port : 0;
}

/* ============================================================================
 * Internal Prometheus metrics registration (Item #9)
 * ========================================================================= */

distric_err_t obs_server_register_internal_metrics(obs_server_t* server,
                                                    metrics_registry_t* registry) {
    if (!server || !registry) return DISTRIC_ERR_INVALID_ARG;
    if (server->metrics_registered) return DISTRIC_OK;

    metrics_register_counter(registry,
        "distric_internal_http_requests_total",
        "Total HTTP requests handled by the observability server",
        NULL, 0, &server->internal_metrics.requests_total);

    metrics_register_counter(registry,
        "distric_internal_http_errors_4xx_total",
        "Total HTTP 4xx errors (client errors: 400, 404, 405)",
        NULL, 0, &server->internal_metrics.errors_4xx_total);

    metrics_register_counter(registry,
        "distric_internal_http_errors_5xx_total",
        "Total HTTP 5xx errors (server errors: 500, 503)",
        NULL, 0, &server->internal_metrics.errors_5xx_total);

    metrics_register_gauge(registry,
        "distric_internal_http_active_connections",
        "Approximate number of connections currently being handled",
        NULL, 0, &server->internal_metrics.active_connections);

    server->metrics_registered = true;
    return DISTRIC_OK;
}