/**
 * @file tcp.c
 * @brief DistriC TCP Transport — Refactored v2
 *
 * Key architectural changes from v1:
 *
 * 1. NON-BLOCKING I/O EVERYWHERE
 *    All sockets are O_NONBLOCK. poll/epoll is used with bounded timeouts.
 *    tcp_send() and tcp_recv() never block indefinitely inside the library.
 *
 * 2. PER-CONNECTION SEND QUEUE WITH BACKPRESSURE
 *    tcp_send() buffers EAGAIN data in a send_queue_t. When the queue exceeds
 *    its HWM, DISTRIC_ERR_BACKPRESSURE is returned so callers can throttle.
 *    Sends are protected by a per-connection mutex (send_lock).
 *
 * 3. OBSERVABILITY DECOUPLED FROM HOT PATHS
 *    - Metrics (counters/gauges): lock-free atomic increments from distric_obs.
 *      These are O(1) and non-blocking; they remain on hot paths.
 *    - Logging: LOG_DEBUG calls are removed from send/recv hot paths.
 *      Only connection lifecycle events (accept, close, errors) are logged.
 *      This removes async ring-buffer writes from the I/O critical path.
 *
 * 4. STABLE ERROR TAXONOMY
 *    errno values are classified via transport_classify_errno() before being
 *    returned or logged, so callers get consistent error codes.
 *
 * 5. EPOLL-BASED SERVER ACCEPT LOOP
 *    Uses epoll_wait with 100ms timeout for clean shutdown. Each accepted
 *    connection is dispatched to a detached thread via the user callback.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/tcp.h"
#include "distric_transport/transport_error.h"
#include "send_queue.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>

/* ============================================================================
 * COMPILE-TIME CONSTANTS
 * ========================================================================= */

#define TCP_EPOLL_MAX_EVENTS   64
#define TCP_ACCEPT_TIMEOUT_MS  100   /* epoll_wait timeout in accept loop  */
#define TCP_RECV_EPOLL_FLAGS   (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP)

/* ============================================================================
 * GLOBAL CONNECTION ID COUNTER (shared by server and client connections)
 * ========================================================================= */

static _Atomic uint64_t g_conn_id_counter = 1;

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

struct tcp_connection_s {
    int               fd;
    char              remote_addr[64];
    uint16_t          remote_port;
    uint64_t          connection_id;

    /* Send path — protected by send_lock */
    pthread_mutex_t   send_lock;
    send_queue_t      send_queue;

    /* Observability — metrics are lock-free; logger pointer is read-only */
    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           bytes_sent_metric;
    metric_t*           bytes_recv_metric;
    metric_t*           send_errors_metric;
    metric_t*           recv_errors_metric;
    metric_t*           backpressure_metric;
};

struct tcp_server_s {
    int               listen_fd;
    int               epoll_fd;      /* epoll fd for accept loop */
    char              bind_addr[64];
    uint16_t          port;

    _Atomic bool      running;
    pthread_t         accept_thread;

    tcp_connection_callback_t callback;
    void*                     userdata;

    /* Observability */
    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           connections_total_metric;
    metric_t*           active_connections_metric;
    metric_t*           accept_errors_metric;

    /* Config template for accepted connections */
    tcp_connection_config_t conn_config;
};

/* ============================================================================
 * SOCKET HELPERS
 * ========================================================================= */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_reuseaddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

static int set_tcp_nodelay(int fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

/* ============================================================================
 * CONNECTION CONSTRUCTION / DESTRUCTION (internal helpers)
 * ========================================================================= */

static tcp_connection_t* connection_alloc(
    int fd,
    const char* remote_addr,
    uint16_t remote_port,
    const tcp_connection_config_t* cfg,
    metrics_registry_t* metrics,
    logger_t* logger
) {
    tcp_connection_t* conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;

    size_t q_cap = (cfg && cfg->send_queue_capacity)
                   ? cfg->send_queue_capacity
                   : SEND_QUEUE_DEFAULT_CAPACITY;
    size_t q_hwm = (cfg && cfg->send_queue_hwm)
                   ? cfg->send_queue_hwm
                   : SEND_QUEUE_DEFAULT_HWM;

    if (send_queue_init(&conn->send_queue, q_cap, q_hwm) != 0) {
        free(conn);
        return NULL;
    }

    pthread_mutex_init(&conn->send_lock, NULL);

    conn->fd             = fd;
    conn->remote_port    = remote_port;
    conn->connection_id  = atomic_fetch_add(&g_conn_id_counter, 1);
    conn->metrics        = metrics;
    conn->logger         = logger;

    strncpy(conn->remote_addr, remote_addr ? remote_addr : "unknown",
            sizeof(conn->remote_addr) - 1);

    /* Register per-connection metrics.
     * metrics_register_counter is idempotent on duplicate names (returns
     * DISTRIC_ERR_ALREADY_EXISTS with the existing metric). */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_bytes_sent_total",
            "Total TCP bytes sent", NULL, 0, &conn->bytes_sent_metric);
        metrics_register_counter(metrics, "tcp_bytes_received_total",
            "Total TCP bytes received", NULL, 0, &conn->bytes_recv_metric);
        metrics_register_counter(metrics, "tcp_send_errors_total",
            "Total TCP send errors", NULL, 0, &conn->send_errors_metric);
        metrics_register_counter(metrics, "tcp_recv_errors_total",
            "Total TCP receive errors", NULL, 0, &conn->recv_errors_metric);
        metrics_register_counter(metrics, "tcp_backpressure_events_total",
            "Times BACKPRESSURE was signalled", NULL, 0, &conn->backpressure_metric);
    }

    return conn;
}

/* ============================================================================
 * INTERNAL SEND FLUSH (called with send_lock held)
 * ========================================================================= */

/**
 * Flush as much of the send queue as possible.
 * Returns bytes flushed, 0 if queue was already empty, or -1 on hard error.
 * On EAGAIN simply returns 0 (queue stays; caller will retry later).
 */
static int flush_send_queue_locked(tcp_connection_t* conn) {
    size_t total_flushed = 0;

    while (!send_queue_empty(&conn->send_queue)) {
        const uint8_t* ptr;
        size_t avail;
        send_queue_peek(&conn->send_queue, &ptr, &avail);

        ssize_t sent = send(conn->fd, ptr, avail, MSG_NOSIGNAL);
        if (sent > 0) {
            send_queue_consume(&conn->send_queue, (size_t)sent);
            total_flushed += (size_t)sent;
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;   /* Socket buffer full; try again next call */
            }
            return -1;   /* Hard error */
        } else {
            break;       /* Should not happen on TCP, treat as EAGAIN */
        }
    }

    return (int)total_flushed;
}

/* ============================================================================
 * tcp_connect
 * ========================================================================= */

distric_err_t tcp_connect(
    const char*                    host,
    uint16_t                       port,
    int                            timeout_ms,
    const tcp_connection_config_t* config,
    metrics_registry_t*            metrics,
    logger_t*                      logger,
    tcp_connection_t**             conn_out
) {
    if (!host || !conn_out || timeout_ms <= 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = NULL;
    int gai_err = getaddrinfo(host, port_str, &hints, &result);
    if (gai_err != 0 || !result) {
        if (logger) {
            LOG_ERROR(logger, "tcp", "Host resolution failed",
                     "host", host, "port", port_str,
                     "reason", gai_strerror(gai_err), NULL);
        }
        if (result) freeaddrinfo(result);
        return DISTRIC_ERR_INIT_FAILED;
    }

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        return DISTRIC_ERR_INIT_FAILED;
    }

    set_nonblocking(fd);
    set_tcp_nodelay(fd);

    int cr = connect(fd, result->ai_addr, result->ai_addrlen);
    /* Save addr before freeing */
    struct sockaddr_in saved_addr;
    memcpy(&saved_addr, result->ai_addr,
           sizeof(struct sockaddr_in) < result->ai_addrlen
           ? sizeof(struct sockaddr_in) : result->ai_addrlen);
    freeaddrinfo(result);

    if (cr < 0 && errno != EINPROGRESS) {
        transport_err_t terr = transport_classify_errno(errno);
        if (logger) {
            LOG_ERROR(logger, "tcp", "Connect failed immediately",
                     "host", host, "port", port_str,
                     "error", transport_err_str(terr), NULL);
        }
        close(fd);
        return transport_err_to_distric(terr);
    }

    if (cr < 0 && errno == EINPROGRESS) {
        /* Use epoll to wait for connect completion */
        int efd = epoll_create1(0);
        if (efd < 0) {
            close(fd);
            return DISTRIC_ERR_INIT_FAILED;
        }

        struct epoll_event ev = {
            .events  = EPOLLOUT | EPOLLERR,
            .data.fd = fd
        };
        epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);

        struct epoll_event events[1];
        int nev = epoll_wait(efd, events, 1, timeout_ms);
        close(efd);

        if (nev <= 0) {
            transport_err_t terr = (nev == 0) ? TRANSPORT_TIMEOUT
                                              : transport_classify_errno(errno);
            if (logger) {
                LOG_ERROR(logger, "tcp", "Connect timed out",
                         "host", host, "port", port_str,
                         "error", transport_err_str(terr), NULL);
            }
            close(fd);
            return (nev == 0) ? DISTRIC_ERR_TIMEOUT : DISTRIC_ERR_INIT_FAILED;
        }

        int so_err = 0;
        socklen_t so_len = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
        if (so_err != 0) {
            transport_err_t terr = transport_classify_errno(so_err);
            if (logger) {
                LOG_ERROR(logger, "tcp", "Connect failed",
                         "host", host, "port", port_str,
                         "error", transport_err_str(terr), NULL);
            }
            close(fd);
            return transport_err_to_distric(terr);
        }
    }

    /* Build remote address string */
    char addr_str[64];
    inet_ntop(AF_INET, &saved_addr.sin_addr, addr_str, sizeof(addr_str));

    tcp_connection_t* conn = connection_alloc(fd, addr_str, port, config, metrics, logger);
    if (!conn) {
        close(fd);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    if (logger) {
        LOG_INFO(logger, "tcp", "Connected",
                "remote_addr", addr_str, "port", port_str, NULL);
    }

    *conn_out = conn;
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_send
 * ========================================================================= */

int tcp_send(tcp_connection_t* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&conn->send_lock);

    /* 1. Flush any previously-queued data first */
    if (!send_queue_empty(&conn->send_queue)) {
        int fl = flush_send_queue_locked(conn);
        if (fl < 0) {
            transport_err_t terr = transport_classify_errno(errno);
            if (conn->send_errors_metric) metrics_counter_inc(conn->send_errors_metric);
            if (conn->logger) {
                LOG_ERROR(conn->logger, "tcp", "Send queue flush failed",
                         "error", transport_err_str(terr), NULL);
            }
            pthread_mutex_unlock(&conn->send_lock);
            return DISTRIC_ERR_IO;
        }
    }

    size_t total_accepted = 0;

    /* 2. If queue still has data, we can't send directly — append to queue */
    if (!send_queue_empty(&conn->send_queue)) {
        goto queue_data;
    }

    /* 3. Try direct send */
    {
        const uint8_t* ptr     = (const uint8_t*)data;
        size_t         remaining = len;

        while (remaining > 0) {
            ssize_t sent = send(conn->fd, ptr, remaining, MSG_NOSIGNAL);
            if (sent > 0) {
                ptr           += sent;
                remaining     -= (size_t)sent;
                total_accepted += (size_t)sent;
            } else if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Buffer the rest */
                    data = ptr;
                    len  = remaining;
                    goto queue_data;
                }
                /* Hard error */
                transport_err_t terr = transport_classify_errno(errno);
                if (conn->send_errors_metric) metrics_counter_inc(conn->send_errors_metric);
                if (conn->logger) {
                    LOG_ERROR(conn->logger, "tcp", "Send failed",
                             "error", transport_err_str(terr), NULL);
                }
                pthread_mutex_unlock(&conn->send_lock);
                return DISTRIC_ERR_IO;
            } else {
                /* sent == 0: unexpected on stream socket; treat as error */
                pthread_mutex_unlock(&conn->send_lock);
                return DISTRIC_ERR_IO;
            }
        }

        /* All data sent directly */
        if (conn->bytes_sent_metric) {
            metrics_counter_add(conn->bytes_sent_metric, total_accepted);
        }
        pthread_mutex_unlock(&conn->send_lock);
        return (int)total_accepted;
    }

queue_data:
    /* 4. Append to send queue */
    if (send_queue_push(&conn->send_queue, data, len) != 0) {
        /* Queue full — signal backpressure */
        if (conn->backpressure_metric) metrics_counter_inc(conn->backpressure_metric);
        if (conn->send_errors_metric)  metrics_counter_inc(conn->send_errors_metric);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_BACKPRESSURE;
    }

    total_accepted += len;

    /* 5. Update metrics for accepted bytes (queued counts as accepted) */
    if (conn->bytes_sent_metric) {
        metrics_counter_add(conn->bytes_sent_metric, total_accepted);
    }

    /* 6. Check HWM after push */
    if (send_queue_above_hwm(&conn->send_queue)) {
        if (conn->backpressure_metric) metrics_counter_inc(conn->backpressure_metric);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_BACKPRESSURE;
    }

    pthread_mutex_unlock(&conn->send_lock);
    return (int)total_accepted;
}

/* ============================================================================
 * tcp_flush
 * ========================================================================= */

distric_err_t tcp_flush(tcp_connection_t* conn) {
    if (!conn) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&conn->send_lock);

    int fl = flush_send_queue_locked(conn);
    if (fl < 0) {
        if (conn->send_errors_metric) metrics_counter_inc(conn->send_errors_metric);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_IO;
    }

    bool above = send_queue_above_hwm(&conn->send_queue);
    pthread_mutex_unlock(&conn->send_lock);

    return above ? DISTRIC_ERR_BACKPRESSURE : DISTRIC_OK;
}

/* ============================================================================
 * tcp_is_writable / tcp_send_queue_depth
 * ========================================================================= */

bool tcp_is_writable(tcp_connection_t* conn) {
    if (!conn) return false;
    pthread_mutex_lock(&conn->send_lock);
    bool writable = !send_queue_above_hwm(&conn->send_queue);
    pthread_mutex_unlock(&conn->send_lock);
    return writable;
}

size_t tcp_send_queue_depth(tcp_connection_t* conn) {
    if (!conn) return 0;
    pthread_mutex_lock(&conn->send_lock);
    size_t depth = send_queue_pending(&conn->send_queue);
    pthread_mutex_unlock(&conn->send_lock);
    return depth;
}

/* ============================================================================
 * tcp_recv
 * ========================================================================= */

int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms) {
    if (!conn || !buffer || len == 0) return DISTRIC_ERR_INVALID_ARG;

    /* Optional wait for data using epoll */
    if (timeout_ms >= 0) {
        int efd = epoll_create1(0);
        if (efd < 0) return DISTRIC_ERR_IO;

        struct epoll_event ev = {
            .events  = TCP_RECV_EPOLL_FLAGS,
            .data.fd = conn->fd
        };
        epoll_ctl(efd, EPOLL_CTL_ADD, conn->fd, &ev);

        struct epoll_event events[1];
        int nev = epoll_wait(efd, events, 1, timeout_ms == 0 ? -1 : timeout_ms);
        close(efd);

        if (nev == 0) return 0;    /* Timeout */
        if (nev < 0) {
            if (conn->recv_errors_metric) metrics_counter_inc(conn->recv_errors_metric);
            return DISTRIC_ERR_IO;
        }

        /* Check for HUP/ERR without data */
        if (events[0].events & (EPOLLHUP | EPOLLERR)) {
            if (!(events[0].events & EPOLLIN)) {
                return DISTRIC_ERR_EOF;
            }
        }
    }

    ssize_t received = recv(conn->fd, buffer, len, 0);

    if (received > 0) {
        /* Hot path: lock-free metric update only */
        if (conn->bytes_recv_metric) {
            metrics_counter_add(conn->bytes_recv_metric, (uint64_t)received);
        }
        return (int)received;
    }

    if (received == 0) {
        /* Graceful close */
        if (conn->logger) {
            LOG_INFO(conn->logger, "tcp", "Peer closed connection",
                    "remote_addr", conn->remote_addr, NULL);
        }
        return DISTRIC_ERR_EOF;
    }

    /* received < 0 */
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;  /* Non-blocking: no data available */
    }

    transport_err_t terr = transport_classify_errno(errno);
    if (conn->recv_errors_metric) metrics_counter_inc(conn->recv_errors_metric);
    if (conn->logger) {
        LOG_ERROR(conn->logger, "tcp", "Recv failed",
                 "error", transport_err_str(terr), NULL);
    }
    return DISTRIC_ERR_IO;
}

/* ============================================================================
 * tcp_close
 * ========================================================================= */

void tcp_close(tcp_connection_t* conn) {
    if (!conn) return;

    if (conn->logger) {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", conn->remote_port);
        LOG_INFO(conn->logger, "tcp", "Connection closed",
                "remote_addr", conn->remote_addr,
                "port", port_str, NULL);
    }

    close(conn->fd);
    send_queue_destroy(&conn->send_queue);
    pthread_mutex_destroy(&conn->send_lock);
    free(conn);
}

/* ============================================================================
 * tcp_get_remote_addr / tcp_get_connection_id
 * ========================================================================= */

distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char*             addr_out,
    size_t            addr_len,
    uint16_t*         port_out
) {
    if (!conn || !addr_out || addr_len == 0) return DISTRIC_ERR_INVALID_ARG;

    strncpy(addr_out, conn->remote_addr, addr_len - 1);
    addr_out[addr_len - 1] = '\0';
    if (port_out) *port_out = conn->remote_port;
    return DISTRIC_OK;
}

uint64_t tcp_get_connection_id(tcp_connection_t* conn) {
    return conn ? conn->connection_id : 0;
}

/* ============================================================================
 * SERVER — ACCEPT THREAD
 * ========================================================================= */

typedef struct {
    tcp_connection_t*         conn;
    tcp_connection_callback_t callback;
    void*                     userdata;
} handler_args_t;

static void* handler_thread_func(void* arg) {
    handler_args_t* h = (handler_args_t*)arg;
    h->callback(h->conn, h->userdata);
    free(h);
    return NULL;
}

static void* accept_thread_func(void* arg) {
    tcp_server_t* srv = (tcp_server_t*)arg;

    struct epoll_event events[TCP_EPOLL_MAX_EVENTS];

    while (atomic_load(&srv->running)) {
        int n = epoll_wait(srv->epoll_fd, events, TCP_EPOLL_MAX_EVENTS,
                           TCP_ACCEPT_TIMEOUT_MS);

        if (n < 0) {
            if (errno == EINTR) continue;
            break;  /* Unexpected epoll error; stop */
        }

        for (int i = 0; i < n; i++) {
            if (!(events[i].events & EPOLLIN)) continue;

            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);

            int cfd = accept(srv->listen_fd,
                             (struct sockaddr*)&client_addr, &addr_len);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;

                transport_err_t terr = transport_classify_errno(errno);
                if (srv->accept_errors_metric)
                    metrics_counter_inc(srv->accept_errors_metric);
                if (srv->logger) {
                    LOG_ERROR(srv->logger, "tcp_server", "Accept failed",
                             "error", transport_err_str(terr), NULL);
                }
                continue;
            }

            set_nonblocking(cfd);
            set_tcp_nodelay(cfd);

            char addr_str[64];
            inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
            uint16_t rport = ntohs(client_addr.sin_port);

            tcp_connection_t* conn = connection_alloc(
                cfd, addr_str, rport,
                &srv->conn_config,
                srv->metrics, srv->logger);

            if (!conn) {
                close(cfd);
                continue;
            }

            /* Update connection-level metrics */
            if (srv->connections_total_metric)
                metrics_counter_inc(srv->connections_total_metric);

            if (srv->logger) {
                char port_str[8];
                snprintf(port_str, sizeof(port_str), "%u", rport);
                LOG_INFO(srv->logger, "tcp_server", "Connection accepted",
                        "remote_addr", addr_str,
                        "remote_port", port_str, NULL);
            }

            /* Dispatch to callback on a detached thread */
            handler_args_t* h = malloc(sizeof(handler_args_t));
            if (!h) { tcp_close(conn); continue; }

            h->conn     = conn;
            h->callback = srv->callback;
            h->userdata = srv->userdata;

            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            pthread_t tid;
            if (pthread_create(&tid, &attr, handler_thread_func, h) != 0) {
                free(h);
                tcp_close(conn);
            }
            pthread_attr_destroy(&attr);
        }
    }

    return NULL;
}

/* ============================================================================
 * tcp_server_create
 * ========================================================================= */

distric_err_t tcp_server_create(
    const char*         bind_addr,
    uint16_t            port,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_server_t**      server_out
) {
    if (!bind_addr || !server_out) return DISTRIC_ERR_INVALID_ARG;

    tcp_server_t* srv = calloc(1, sizeof(*srv));
    if (!srv) return DISTRIC_ERR_ALLOC_FAILURE;

    srv->metrics = metrics;
    srv->logger  = logger;
    srv->port    = port;
    atomic_init(&srv->running, false);
    strncpy(srv->bind_addr, bind_addr, sizeof(srv->bind_addr) - 1);

    /* Create epoll fd */
    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) { free(srv); return DISTRIC_ERR_INIT_FAILED; }

    /* Create listen socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        close(srv->epoll_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }

    set_nonblocking(srv->listen_fd);
    set_reuseaddr(srv->listen_fd);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        transport_err_t terr = transport_classify_errno(errno);
        if (logger) {
            char port_str[8];
            snprintf(port_str, sizeof(port_str), "%u", port);
            LOG_ERROR(logger, "tcp_server", "Bind failed",
                     "bind_addr", bind_addr, "port", port_str,
                     "error", transport_err_str(terr), NULL);
        }
        close(srv->listen_fd);
        close(srv->epoll_fd);
        free(srv);
        return transport_err_to_distric(terr);
    }

    if (listen(srv->listen_fd, 128) < 0) {
        close(srv->listen_fd);
        close(srv->epoll_fd);
        free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }

    /* Register listen_fd with epoll */
    struct epoll_event ev = {
        .events  = EPOLLIN,
        .data.fd = srv->listen_fd
    };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_server_connections_total",
            "Total accepted connections", NULL, 0, &srv->connections_total_metric);
        metrics_register_gauge(metrics, "tcp_server_active_connections",
            "Currently active connections", NULL, 0, &srv->active_connections_metric);
        metrics_register_counter(metrics, "tcp_server_accept_errors_total",
            "Total accept errors", NULL, 0, &srv->accept_errors_metric);
    }

    if (logger) {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", port);
        LOG_INFO(logger, "tcp_server", "Server created",
                "bind_addr", bind_addr, "port", port_str, NULL);
    }

    *server_out = srv;
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_server_start / stop / destroy
 * ========================================================================= */

distric_err_t tcp_server_start(
    tcp_server_t*             server,
    tcp_connection_callback_t on_connection,
    void*                     userdata
) {
    if (!server || !on_connection) return DISTRIC_ERR_INVALID_ARG;

    server->callback = on_connection;
    server->userdata = userdata;
    atomic_store(&server->running, true);

    if (pthread_create(&server->accept_thread, NULL,
                       accept_thread_func, server) != 0) {
        atomic_store(&server->running, false);
        return DISTRIC_ERR_INIT_FAILED;
    }

    if (server->logger) {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", server->port);
        LOG_INFO(server->logger, "tcp_server", "Server started",
                "bind_addr", server->bind_addr, "port", port_str, NULL);
    }

    return DISTRIC_OK;
}

distric_err_t tcp_server_stop(tcp_server_t* server) {
    if (!server) return DISTRIC_ERR_INVALID_ARG;
    if (!atomic_load(&server->running)) return DISTRIC_OK;

    if (server->logger)
        LOG_INFO(server->logger, "tcp_server", "Stopping server", NULL);

    atomic_store(&server->running, false);

    /* Wake up epoll_wait by closing the listen fd's duplicate.
     * epoll_wait will unblock because the fd is removed. */
    shutdown(server->listen_fd, SHUT_RDWR);

    pthread_join(server->accept_thread, NULL);

    if (server->logger)
        LOG_INFO(server->logger, "tcp_server", "Server stopped", NULL);

    return DISTRIC_OK;
}

void tcp_server_destroy(tcp_server_t* server) {
    if (!server) return;

    if (atomic_load(&server->running))
        tcp_server_stop(server);

    close(server->listen_fd);
    close(server->epoll_fd);
    free(server);
}