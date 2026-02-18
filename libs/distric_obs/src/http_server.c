/*
 * http_server.c — DistriC Observability Library — HTTP Server
 *
 * Security model (Improvement #3):
 *   - SO_RCVTIMEO / SO_SNDTIMEO enforced per accepted socket.
 *   - Max request line: HTTP_MAX_REQUEST_BYTES.
 *   - Only GET requests accepted.
 *   - Path traversal (../) rejected with 400.
 *   - Response bodies capped at max_response_bytes.
 *   - Export data is COPIED from registries before any formatting;
 *     the server holds NO registry locks while writing to the network.
 *   - The HTTP server is single-purpose (observability endpoints only).
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
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <pthread.h>

/* ============================================================================
 * Socket helpers
 * ========================================================================= */

static void apply_socket_timeouts(int fd, uint32_t read_ms, uint32_t write_ms) {
    struct timeval rtv = {
        .tv_sec  = read_ms / 1000,
        .tv_usec = (read_ms % 1000) * 1000
    };
    struct timeval wtv = {
        .tv_sec  = write_ms / 1000,
        .tv_usec = (write_ms % 1000) * 1000
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv));
}

/* ============================================================================
 * Request parsing (strict and minimal)
 * ========================================================================= */

/*
 * Reads up to HTTP_MAX_REQUEST_BYTES from client_fd, finds the request line,
 * validates method and path.
 * Returns 0 on success, -1 on parse error or timeout.
 */
static int parse_request(int client_fd, http_request_t* req) {
    char buf[HTTP_MAX_REQUEST_BYTES + 1];
    size_t total = 0;
    int end_found = 0;

    /* Read until we see \r\n or run out of space */
    while (total < HTTP_MAX_REQUEST_BYTES) {
        ssize_t n = recv(client_fd, buf + total, HTTP_MAX_REQUEST_BYTES - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n")) { end_found = 1; break; }
    }
    if (!end_found) return -1;

    /* Parse: METHOD SP PATH SP HTTP/x.y */
    char method[HTTP_MAX_METHOD_LEN];
    char path[HTTP_MAX_PATH_LEN];
    if (sscanf(buf, "%7s %255s", method, path) != 2) return -1;

    /* Only GET accepted */
    if (strncmp(method, "GET", 3) != 0) return -1;

    /* Reject path traversal */
    if (strstr(path, "..") != NULL) return -1;

    strncpy(req->method, method, sizeof(req->method) - 1);
    strncpy(req->path,   path,   sizeof(req->path)   - 1);
    return 0;
}

/* ============================================================================
 * Response formatting and sending
 * ========================================================================= */

static void send_response_str(int fd, int code, const char* code_str,
                               const char* content_type,
                               const char* body, size_t body_len,
                               size_t max_body) {
    /* Hard cap response body */
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
 * Request handlers — each COPIES export data before sending
 * ========================================================================= */

static void handle_metrics(obs_server_t* server, int client_fd) {
    char* body   = NULL;
    size_t bsize = 0;

    /*
     * Copy metrics data (malloc'd buffer) before touching the network.
     * The registry is accessed here but no lock is held across the send().
     */
    distric_err_t err = metrics_export_prometheus(server->metrics, &body, &bsize);

    if (err == DISTRIC_OK && body) {
        send_response_str(client_fd, 200, "OK",
                          "text/plain; version=0.0.4; charset=utf-8",
                          body, bsize, server->max_response_bytes);
        free(body);
    } else {
        SEND_ERROR(client_fd, 500, "Internal Server Error",
                   server->max_response_bytes);
    }
}

static void handle_health_ready(obs_server_t* server, int client_fd) {
    char*   body  = NULL;
    size_t  bsize = 0;

    /* Copy health JSON before touching the network */
    distric_err_t err = health_export_json(server->health, &body, &bsize);
    if (err != DISTRIC_OK || !body) {
        SEND_ERROR(client_fd, 500, "Internal Server Error",
                   server->max_response_bytes);
        return;
    }

    health_status_t overall = health_get_overall_status(server->health);
    int code        = (overall == HEALTH_UP) ? 200 : 503;
    const char* txt = (code == 200)          ? "OK" : "Service Unavailable";

    send_response_str(client_fd, code, txt,
                      "application/json",
                      body, bsize, server->max_response_bytes);
    free(body);
}

static void handle_health_live(obs_server_t* server, int client_fd) {
    (void)server;
    const char* body = "{\"status\":\"UP\"}";
    send_response_str(client_fd, 200, "OK",
                      "application/json",
                      body, strlen(body), server->max_response_bytes);
}

static void handle_not_found(obs_server_t* server, int client_fd) {
    SEND_ERROR(client_fd, 404, "Not Found", server->max_response_bytes);
}

static void handle_method_not_allowed(obs_server_t* server, int client_fd) {
    SEND_ERROR(client_fd, 405, "Method Not Allowed", server->max_response_bytes);
}

/* ============================================================================
 * Accept loop
 * ========================================================================= */

static void handle_connection(obs_server_t* server, int client_fd) {
    apply_socket_timeouts(client_fd, server->read_timeout_ms,
                          server->write_timeout_ms);

    http_request_t req;
    memset(&req, 0, sizeof(req));

    if (parse_request(client_fd, &req) != 0) {
        SEND_ERROR(client_fd, 400, "Bad Request", server->max_response_bytes);
        return;
    }

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

static void* accept_thread_fn(void* arg) {
    obs_server_t* server = (obs_server_t*)arg;

    while (!atomic_load_explicit(&server->shutdown, memory_order_acquire)) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = accept(server->listen_fd,
                                (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (atomic_load_explicit(&server->shutdown, memory_order_acquire))
                break;
            /* Accept failure: brief backoff then retry */
            struct timespec ts = { 0, 10000000 }; /* 10ms */
            nanosleep(&ts, NULL);
            continue;
        }

        handle_connection(server, client_fd);
        close(client_fd);
    }

    return NULL;
}

/* ============================================================================
 * Public API
 * ========================================================================= */

distric_err_t obs_server_init(obs_server_t**      server,
                               uint16_t            port,
                               metrics_registry_t* metrics,
                               health_registry_t*  health) {
    obs_server_config_t cfg = {
        .port    = port,
        .metrics = metrics,
        .health  = health,
    };
    return obs_server_init_with_config(server, &cfg);
}

distric_err_t obs_server_init_with_config(obs_server_t**             out,
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
    atomic_init(&server->shutdown, false);

    /* Create listening socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { free(server); return DISTRIC_ERR_INIT_FAILED; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Apply a short accept timeout so the accept thread can check shutdown */
    struct timeval atv = { .tv_sec = 1, .tv_usec = 0 };
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

    /* Discover actual bound port (useful when config->port == 0) */
    struct sockaddr_in actual;
    socklen_t actual_len = sizeof(actual);
    if (getsockname(fd, (struct sockaddr*)&actual, &actual_len) == 0)
        server->port = ntohs(actual.sin_port);
    else
        server->port = config->port;

    server->listen_fd = fd;

    if (pthread_create(&server->accept_thread, NULL, accept_thread_fn, server) != 0) {
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
    atomic_store_explicit(&server->shutdown, true, memory_order_release);
    /* Close listen fd to unblock accept() */
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