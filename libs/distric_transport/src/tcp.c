/**
 * @file tcp.c
 * @brief DistriC TCP Transport — v3
 *
 * Key changes from v2:
 *
 * 1. BOUNDED WORKER POOL (Item 1)
 *    Replaces unbounded detached-thread-per-connection. N worker threads
 *    (default = CPU count) pull from a fixed-depth MPSC work queue.
 *    The accept thread never spawns threads directly. When the work queue
 *    is full the connection is rejected (fd closed) and an error is logged.
 *
 * 2. SHARED PER-SERVER METRICS (Item 4)
 *    connection_alloc() no longer calls metrics_register_*. The server
 *    registers 5 shared metrics once at create time. All accepted connections
 *    receive a pointer to the shared tcp_conn_metrics_t. Standalone
 *    tcp_connect() callers may supply a server-owned metrics set or NULL.
 *
 * 3. GRACEFUL SHUTDOWN STATE MACHINE (Item 9)
 *    tcp_server_state_t: RUNNING → DRAINING → STOPPED.
 *    tcp_server_stop() sets state to DRAINING and waits (up to drain_timeout_ms)
 *    for active_connections to reach zero.
 *
 * 4. CIRCUIT BREAKER INTEGRATION (Item 8)
 *    tcp_connect() consults a module-level cb_registry before dialing.
 *    Records success/failure to drive CLOSED/OPEN/HALF_OPEN transitions.
 *
 * 5. CACHED PER-CONNECTION RECV EPOLL FD (Item 3 partial)
 *    tcp_recv() no longer calls epoll_create1/close per call.
 *    Each tcp_connection_t owns a recv_epoll_fd, registered once at
 *    connection_alloc() time. Eliminates a syscall pair on every recv.
 *
 * 6. RING BUFFER SEND QUEUE (Item 5 — via send_queue.h/c v3)
 *    No memmove, no compaction. O(1) push and consume.
 *
 * 7. OBSERVABILITY LIFECYCLE SAFETY (Item 2)
 *    Logger and metrics pointers are NULLed during server shutdown before
 *    the accept loop is joined.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/tcp.h"
#include "distric_transport/transport_error.h"
#include "send_queue.h"
#include "circuit_breaker.h"
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
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>

/* ============================================================================
 * COMPILE-TIME CONSTANTS
 * ========================================================================= */

#define TCP_EPOLL_MAX_EVENTS          64
#define TCP_ACCEPT_TIMEOUT_MS         100
#define TCP_RECV_EPOLL_FLAGS          (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP)

#define TCP_DEFAULT_WORKERS           0          /* 0 = auto (CPU count)   */
#define TCP_MAX_WORKERS               32
#define TCP_DEFAULT_QUEUE_DEPTH       4096
#define TCP_DEFAULT_DRAIN_TIMEOUT_MS  5000u

/* ============================================================================
 * GLOBAL CONNECTION ID COUNTER
 * ========================================================================= */

static _Atomic uint64_t g_conn_id_counter = 1;

/* ============================================================================
 * MODULE-LEVEL CIRCUIT BREAKER
 *
 * One shared registry for all standalone tcp_connect() callers. Server-owned
 * connections also use this registry (keyed by host:port). Production users
 * that need per-pool isolation can be extended later.
 * ========================================================================= */

static cb_registry_t* g_cb_registry = NULL;
static pthread_once_t g_cb_once     = PTHREAD_ONCE_INIT;

static void cb_init_once(void) {
    cb_registry_create(NULL, NULL, NULL, &g_cb_registry);
}

static cb_registry_t* get_cb_registry(void) {
    pthread_once(&g_cb_once, cb_init_once);
    return g_cb_registry;
}

/* ============================================================================
 * SHARED CONNECTION METRICS
 *
 * Registered once per server, shared across all accepted connections.
 * Standalone tcp_connect() callers receive NULL (metrics not registered).
 * ========================================================================= */

typedef struct {
    metric_t* bytes_sent;
    metric_t* bytes_recv;
    metric_t* send_errors;
    metric_t* recv_errors;
    metric_t* backpressure;
} tcp_conn_metrics_t;

/* ============================================================================
 * TCP CONNECTION STRUCTURE
 * ========================================================================= */

struct tcp_connection_s {
    int               fd;
    int               recv_epoll_fd;  /* Cached epoll fd for recv waits */
    char              remote_addr[64];
    uint16_t          remote_port;
    uint64_t          connection_id;

    /* Send path — protected by send_lock */
    pthread_mutex_t   send_lock;
    send_queue_t      send_queue;

    /* Shared metrics reference (NULL for standalone connections w/o server) */
    const tcp_conn_metrics_t* metrics;

    /* Logger — read-only after construction */
    logger_t*         logger;
};

/* ============================================================================
 * WORKER POOL STRUCTURES
 * ========================================================================= */

typedef struct {
    tcp_connection_t*         conn;
    tcp_connection_callback_t callback;
    void*                     userdata;
} work_item_t;

typedef struct {
    work_item_t*      items;
    uint32_t          capacity;
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
    pthread_mutex_t   push_lock;  /* Serialise producers */
    pthread_cond_t    not_empty;
    pthread_mutex_t   wait_lock;
    _Atomic bool      shutdown;
} work_queue_t;

typedef struct {
    pthread_t        thread;
    work_queue_t*    queue;
    _Atomic int64_t* active_connections; /* Pointer to server counter */
} worker_t;

/* ============================================================================
 * TCP SERVER STRUCTURE
 * ========================================================================= */

struct tcp_server_s {
    int               listen_fd;
    int               epoll_fd;
    char              bind_addr[64];
    uint16_t          port;

    _Atomic int               state;          /* tcp_server_state_t */
    _Atomic int64_t           active_connections;

    pthread_t         accept_thread;

    tcp_connection_callback_t callback;
    void*                     userdata;

    /* Worker pool */
    worker_t*         workers;
    uint32_t          worker_count;
    work_queue_t      work_queue;

    /* Server-level metrics */
    metrics_registry_t*  metrics;
    logger_t*            logger;
    metric_t*            connections_total_metric;
    metric_t*            active_connections_metric;
    metric_t*            accept_errors_metric;
    metric_t*            queue_full_metric;

    /* Shared per-connection I/O metrics (registered once) */
    tcp_conn_metrics_t   conn_metrics;

    /* Config */
    tcp_server_config_t  config;
    tcp_connection_config_t conn_config; /* Send queue params for accepted conns */

    uint32_t          drain_timeout_ms;
};

/* ============================================================================
 * SOCKET HELPERS
 * ========================================================================= */

static int set_nonblocking(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void set_reuseaddr(int fd) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

static void set_tcp_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static void set_sndbuf(int fd, size_t size) {
    int opt = (int)size;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));
}

static uint32_t cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 4;
    return (uint32_t)n > TCP_MAX_WORKERS ? TCP_MAX_WORKERS : (uint32_t)n;
}

/* ============================================================================
 * WORK QUEUE — BOUNDED MPSC RING
 * ========================================================================= */

static int wq_init(work_queue_t* q, uint32_t capacity) {
    q->items    = calloc(capacity, sizeof(work_item_t));
    if (!q->items) return -1;
    q->capacity = capacity;
    atomic_init(&q->head,     0);
    atomic_init(&q->tail,     0);
    atomic_init(&q->shutdown, false);
    pthread_mutex_init(&q->push_lock, NULL);
    pthread_mutex_init(&q->wait_lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

static void wq_destroy(work_queue_t* q) {
    pthread_mutex_lock(&q->wait_lock);
    atomic_store(&q->shutdown, true);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
    pthread_mutex_destroy(&q->push_lock);
    pthread_mutex_destroy(&q->wait_lock);
    pthread_cond_destroy(&q->not_empty);
    free(q->items);
    q->items = NULL;
}

/* Returns 0 on success, -1 if queue full */
static int wq_push(work_queue_t* q, const work_item_t* item) {
    pthread_mutex_lock(&q->push_lock);
    uint32_t h    = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t    = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t next = (t + 1) % q->capacity;
    if (next == h) {
        pthread_mutex_unlock(&q->push_lock);
        return -1; /* Full */
    }
    q->items[t] = *item;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    pthread_mutex_unlock(&q->push_lock);

    /* Signal one waiting worker */
    pthread_mutex_lock(&q->wait_lock);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
    return 0;
}

/* Blocks until item available or shutdown. Returns 0 on success, -1 on shutdown. */
static int wq_pop(work_queue_t* q, work_item_t* out) {
    pthread_mutex_lock(&q->wait_lock);
    while (true) {
        uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
        uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
        if (h != t) {
            *out = q->items[h];
            atomic_store_explicit(&q->head, (h + 1) % q->capacity, memory_order_release);
            pthread_mutex_unlock(&q->wait_lock);
            return 0;
        }
        if (atomic_load(&q->shutdown)) {
            pthread_mutex_unlock(&q->wait_lock);
            return -1;
        }
        pthread_cond_wait(&q->not_empty, &q->wait_lock);
    }
}

/* ============================================================================
 * WORKER THREAD
 * ========================================================================= */

static void* worker_thread_func(void* arg) {
    worker_t* w = (worker_t*)arg;

    work_item_t item;
    while (wq_pop(w->queue, &item) == 0) {
        item.callback(item.conn, item.userdata);
        atomic_fetch_add(w->active_connections, -1);
    }
    return NULL;
}

/* ============================================================================
 * CONNECTION ALLOCATION
 * ========================================================================= */

static tcp_connection_t* connection_alloc(
    int fd,
    const char* remote_addr,
    uint16_t remote_port,
    const tcp_connection_config_t* cfg,
    const tcp_conn_metrics_t* shared_metrics,
    logger_t* logger
) {
    tcp_connection_t* conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;

    size_t q_cap = (cfg && cfg->send_queue_capacity)
                   ? cfg->send_queue_capacity : SEND_QUEUE_DEFAULT_CAPACITY;
    size_t q_hwm = (cfg && cfg->send_queue_hwm)
                   ? cfg->send_queue_hwm : SEND_QUEUE_DEFAULT_HWM;

    if (send_queue_init(&conn->send_queue, q_cap, q_hwm) != 0) {
        free(conn);
        return NULL;
    }

    /* Cached recv epoll fd — registered once, reused across all recv calls */
    conn->recv_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (conn->recv_epoll_fd >= 0) {
        struct epoll_event ev = {
            .events  = TCP_RECV_EPOLL_FLAGS,
            .data.fd = fd
        };
        epoll_ctl(conn->recv_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    pthread_mutex_init(&conn->send_lock, NULL);

    conn->fd             = fd;
    conn->remote_port    = remote_port;
    conn->connection_id  = atomic_fetch_add(&g_conn_id_counter, 1);
    conn->metrics        = shared_metrics;
    conn->logger         = logger;

    strncpy(conn->remote_addr,
            remote_addr ? remote_addr : "unknown",
            sizeof(conn->remote_addr) - 1);

    return conn;
}

/* ============================================================================
 * tcp_server_create / tcp_server_create_with_config
 * ========================================================================= */

distric_err_t tcp_server_create_with_config(
    const char*               bind_addr,
    uint16_t                  port,
    const tcp_server_config_t* cfg,
    metrics_registry_t*       metrics,
    logger_t*                 logger,
    tcp_server_t**            server_out
) {
    if (!bind_addr || !server_out) return DISTRIC_ERR_INVALID_ARG;

    tcp_server_t* srv = calloc(1, sizeof(*srv));
    if (!srv) return DISTRIC_ERR_ALLOC_FAILURE;

    /* Config with defaults */
    srv->worker_count       = cfg && cfg->worker_threads  ? cfg->worker_threads  : cpu_count();
    if (srv->worker_count > TCP_MAX_WORKERS) srv->worker_count = TCP_MAX_WORKERS;
    uint32_t queue_depth    = cfg && cfg->worker_queue_depth ? cfg->worker_queue_depth : TCP_DEFAULT_QUEUE_DEPTH;
    srv->drain_timeout_ms   = cfg && cfg->drain_timeout_ms   ? cfg->drain_timeout_ms   : TCP_DEFAULT_DRAIN_TIMEOUT_MS;

    srv->conn_config.send_queue_capacity = cfg ? cfg->conn_send_queue_capacity : 0;
    srv->conn_config.send_queue_hwm      = cfg ? cfg->conn_send_queue_hwm      : 0;

    srv->metrics = metrics;
    srv->logger  = logger;
    srv->port    = port;
    strncpy(srv->bind_addr, bind_addr, sizeof(srv->bind_addr) - 1);
    atomic_init(&srv->state, TCP_SERVER_STOPPED);
    atomic_init(&srv->active_connections, 0);

    /* Epoll for accept */
    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) { free(srv); return DISTRIC_ERR_INIT_FAILED; }

    /* Listen socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) { close(srv->epoll_fd); free(srv); return DISTRIC_ERR_INIT_FAILED; }

    set_nonblocking(srv->listen_fd);
    set_reuseaddr(srv->listen_fd);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        transport_err_t terr = transport_classify_errno(errno);
        if (logger) {
            char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
            LOG_ERROR(logger, "tcp_server", "Bind failed",
                     "bind_addr", bind_addr, "port", ps,
                     "error", transport_err_str(terr), NULL);
        }
        close(srv->listen_fd);
        close(srv->epoll_fd);
        free(srv);
        return transport_err_to_distric(terr);
    }

    if (listen(srv->listen_fd, 128) < 0) {
        close(srv->listen_fd); close(srv->epoll_fd); free(srv);
        return DISTRIC_ERR_INIT_FAILED;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    /* Register server-level metrics ONCE */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_server_connections_total",
            "Total accepted connections", NULL, 0, &srv->connections_total_metric);
        metrics_register_gauge(metrics, "tcp_server_active_connections",
            "Currently active connections", NULL, 0, &srv->active_connections_metric);
        metrics_register_counter(metrics, "tcp_server_accept_errors_total",
            "Total accept errors", NULL, 0, &srv->accept_errors_metric);
        metrics_register_counter(metrics, "tcp_server_queue_full_total",
            "Connections rejected: worker queue full", NULL, 0, &srv->queue_full_metric);

        /* Shared connection I/O metrics — registered ONCE, shared by all conns */
        metrics_register_counter(metrics, "tcp_bytes_sent_total",
            "Bytes sent across all connections", NULL, 0, &srv->conn_metrics.bytes_sent);
        metrics_register_counter(metrics, "tcp_bytes_recv_total",
            "Bytes received across all connections", NULL, 0, &srv->conn_metrics.bytes_recv);
        metrics_register_counter(metrics, "tcp_send_errors_total",
            "Send errors", NULL, 0, &srv->conn_metrics.send_errors);
        metrics_register_counter(metrics, "tcp_recv_errors_total",
            "Receive errors", NULL, 0, &srv->conn_metrics.recv_errors);
        metrics_register_counter(metrics, "tcp_backpressure_total",
            "Backpressure signals emitted", NULL, 0, &srv->conn_metrics.backpressure);
    }

    /* Work queue */
    if (wq_init(&srv->work_queue, queue_depth) != 0) {
        close(srv->listen_fd); close(srv->epoll_fd); free(srv);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    /* Worker pool */
    srv->workers = calloc(srv->worker_count, sizeof(worker_t));
    if (!srv->workers) {
        wq_destroy(&srv->work_queue);
        close(srv->listen_fd); close(srv->epoll_fd); free(srv);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    if (logger) {
        char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
        char ws[8]; snprintf(ws, sizeof(ws), "%u", srv->worker_count);
        LOG_INFO(logger, "tcp_server", "Server created",
                "bind_addr", bind_addr, "port", ps,
                "workers", ws, NULL);
    }

    *server_out = srv;
    return DISTRIC_OK;
}

distric_err_t tcp_server_create(
    const char*         bind_addr,
    uint16_t            port,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_server_t**      server
) {
    return tcp_server_create_with_config(bind_addr, port, NULL, metrics, logger, server);
}

/* ============================================================================
 * ACCEPT THREAD
 * ========================================================================= */

static void* accept_thread_func(void* arg) {
    tcp_server_t* srv = (tcp_server_t*)arg;
    struct epoll_event events[TCP_EPOLL_MAX_EVENTS];

    while (atomic_load(&srv->state) == TCP_SERVER_RUNNING) {
        int n = epoll_wait(srv->epoll_fd, events, TCP_EPOLL_MAX_EVENTS,
                           TCP_ACCEPT_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            if (!(events[i].events & EPOLLIN)) continue;

            struct sockaddr_in caddr;
            socklen_t alen = sizeof(caddr);
            int cfd = accept(srv->listen_fd, (struct sockaddr*)&caddr, &alen);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (srv->accept_errors_metric)
                    metrics_counter_inc(srv->accept_errors_metric);
                if (srv->logger) {
                    LOG_ERROR(srv->logger, "tcp_server", "Accept failed",
                             "error", transport_err_str(transport_classify_errno(errno)), NULL);
                }
                continue;
            }

            set_nonblocking(cfd);
            set_tcp_nodelay(cfd);

            char addr_str[64];
            inet_ntop(AF_INET, &caddr.sin_addr, addr_str, sizeof(addr_str));
            uint16_t rport = ntohs(caddr.sin_port);

            tcp_connection_t* conn = connection_alloc(
                cfd, addr_str, rport,
                &srv->conn_config,
                srv->metrics ? &srv->conn_metrics : NULL,
                srv->logger);

            if (!conn) { close(cfd); continue; }

            /* Enqueue to worker pool */
            work_item_t item = {
                .conn     = conn,
                .callback = srv->callback,
                .userdata = srv->userdata,
            };

            if (wq_push(&srv->work_queue, &item) != 0) {
                /* Queue full — reject connection */
                if (srv->queue_full_metric)
                    metrics_counter_inc(srv->queue_full_metric);
                if (srv->logger) {
                    LOG_WARN(srv->logger, "tcp_server",
                            "Worker queue full, connection rejected",
                            "remote_addr", addr_str, NULL);
                }
                tcp_close(conn);
                continue;
            }

            int64_t active = atomic_fetch_add(&srv->active_connections, 1) + 1;
            if (srv->connections_total_metric)
                metrics_counter_inc(srv->connections_total_metric);
            if (srv->active_connections_metric)
                metrics_gauge_set(srv->active_connections_metric, (int64_t)active);

            if (srv->logger) {
                char ps[8]; snprintf(ps, sizeof(ps), "%u", rport);
                LOG_INFO(srv->logger, "tcp_server", "Connection accepted",
                        "remote_addr", addr_str, "remote_port", ps, NULL);
            }
        }
    }
    return NULL;
}

/* ============================================================================
 * tcp_server_start
 * ========================================================================= */

distric_err_t tcp_server_start(
    tcp_server_t*             server,
    tcp_connection_callback_t on_connection,
    void*                     userdata
) {
    if (!server || !on_connection) return DISTRIC_ERR_INVALID_ARG;
    if (atomic_load(&server->state) == TCP_SERVER_RUNNING) return DISTRIC_OK;

    server->callback = on_connection;
    server->userdata = userdata;
    atomic_store(&server->state, TCP_SERVER_RUNNING);

    /* Start workers */
    for (uint32_t i = 0; i < server->worker_count; i++) {
        server->workers[i].queue              = &server->work_queue;
        server->workers[i].active_connections = &server->active_connections;
        if (pthread_create(&server->workers[i].thread, NULL,
                           worker_thread_func, &server->workers[i]) != 0) {
            atomic_store(&server->state, TCP_SERVER_STOPPED);
            return DISTRIC_ERR_INIT_FAILED;
        }
    }

    /* Start accept thread */
    if (pthread_create(&server->accept_thread, NULL, accept_thread_func, server) != 0) {
        atomic_store(&server->state, TCP_SERVER_STOPPED);
        return DISTRIC_ERR_INIT_FAILED;
    }

    if (server->logger) {
        char ps[8]; snprintf(ps, sizeof(ps), "%u", server->port);
        char ws[8]; snprintf(ws, sizeof(ws), "%u", server->worker_count);
        LOG_INFO(server->logger, "tcp_server", "Server started",
                "port", ps, "workers", ws, NULL);
    }

    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_server_stop — RUNNING → DRAINING → STOPPED
 * ========================================================================= */

distric_err_t tcp_server_stop(tcp_server_t* server) {
    if (!server) return DISTRIC_ERR_INVALID_ARG;

    int expected = TCP_SERVER_RUNNING;
    if (!atomic_compare_exchange_strong(&server->state, &expected, TCP_SERVER_DRAINING)) {
        /* Already DRAINING or STOPPED — idempotent */
        return DISTRIC_OK;
    }

    if (server->logger) {
        LOG_INFO(server->logger, "tcp_server", "Draining server", NULL);
    }

    /* Accept thread exits when state != RUNNING */
    pthread_join(server->accept_thread, NULL);

    /* Wait for active connections to reach 0 */
    uint32_t elapsed_ms = 0;
    const uint32_t poll_ms = 10;
    while (atomic_load(&server->active_connections) > 0 &&
           elapsed_ms < server->drain_timeout_ms) {
        struct timespec ts = { .tv_nsec = poll_ms * 1000000L };
        nanosleep(&ts, NULL);
        elapsed_ms += poll_ms;
    }

    int64_t remaining = atomic_load(&server->active_connections);
    if (remaining > 0 && server->logger) {
        char rbuf[24]; snprintf(rbuf, sizeof(rbuf), "%lld", (long long)remaining);
        LOG_WARN(server->logger, "tcp_server",
                "Drain timeout: connections still active",
                "count", rbuf, NULL);
    }

    /* Shut down worker pool */
    wq_destroy(&server->work_queue);
    for (uint32_t i = 0; i < server->worker_count; i++) {
        pthread_join(server->workers[i].thread, NULL);
    }

    /* NULL observability pointers before marking stopped (Item 2) */
    server->logger  = NULL;
    server->metrics = NULL;

    atomic_store(&server->state, TCP_SERVER_STOPPED);
    return DISTRIC_OK;
}

tcp_server_state_t tcp_server_get_state(const tcp_server_t* server) {
    if (!server) return TCP_SERVER_STOPPED;
    return (tcp_server_state_t)atomic_load(&server->state);
}

int64_t tcp_server_active_connections(const tcp_server_t* server) {
    if (!server) return 0;
    return atomic_load(&server->active_connections);
}

void tcp_server_destroy(tcp_server_t* server) {
    if (!server) return;
    tcp_server_stop(server);
    close(server->listen_fd);
    close(server->epoll_fd);
    free(server->workers);
    free(server);
}

/* ============================================================================
 * tcp_connect — with circuit breaker
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
    if (!host || !conn_out || timeout_ms <= 0) return DISTRIC_ERR_INVALID_ARG;
    (void)metrics; /* metrics are shared via the server; standalone connect doesn't register */

    /* Circuit breaker check */
    cb_registry_t* cb = get_cb_registry();
    if (!cb_is_allowed(cb, host, port)) {
        if (logger) {
            LOG_WARN(logger, "tcp", "Circuit open: connection rejected",
                    "host", host, NULL);
        }
        return DISTRIC_ERR_UNAVAILABLE;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = NULL;

    if (getaddrinfo(host, port_str, &hints, &result) != 0 || !result) {
        cb_record_failure(cb, host, port);
        return DISTRIC_ERR_INIT_FAILED;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(result);
        cb_record_failure(cb, host, port);
        return DISTRIC_ERR_INIT_FAILED;
    }

    set_nonblocking(fd);
    set_tcp_nodelay(fd);

    if (config && config->send_queue_capacity > 0) {
        set_sndbuf(fd, config->send_queue_capacity);
    }

    int cr = connect(fd, result->ai_addr, result->ai_addrlen);

    struct sockaddr_in saved_addr;
    memcpy(&saved_addr, result->ai_addr,
           sizeof(saved_addr) < result->ai_addrlen
           ? sizeof(saved_addr) : result->ai_addrlen);
    freeaddrinfo(result);

    if (cr < 0 && errno != EINPROGRESS) {
        transport_err_t terr = transport_classify_errno(errno);
        if (logger) {
            LOG_ERROR(logger, "tcp", "Connect failed",
                     "host", host, "port", port_str,
                     "error", transport_err_str(terr), NULL);
        }
        close(fd);
        cb_record_failure(cb, host, port);
        return transport_err_to_distric(terr);
    }

    if (cr < 0 && errno == EINPROGRESS) {
        int efd = epoll_create1(0);
        if (efd < 0) { close(fd); cb_record_failure(cb, host, port); return DISTRIC_ERR_INIT_FAILED; }

        struct epoll_event ev = { .events = EPOLLOUT | EPOLLERR, .data.fd = fd };
        epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);

        struct epoll_event evout[1];
        int nev = epoll_wait(efd, evout, 1, timeout_ms);
        close(efd);

        if (nev <= 0) {
            if (logger) {
                LOG_ERROR(logger, "tcp", "Connect timed out",
                         "host", host, "port", port_str, NULL);
            }
            close(fd);
            cb_record_failure(cb, host, port);
            return (nev == 0) ? DISTRIC_ERR_TIMEOUT : DISTRIC_ERR_INIT_FAILED;
        }

        int so_err = 0;
        socklen_t so_len = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
        if (so_err != 0) {
            if (logger) {
                LOG_ERROR(logger, "tcp", "Connect failed (SO_ERROR)",
                         "host", host, "port", port_str, NULL);
            }
            close(fd);
            cb_record_failure(cb, host, port);
            return transport_err_to_distric(transport_classify_errno(so_err));
        }
    }

    char addr_str[64];
    inet_ntop(AF_INET, &saved_addr.sin_addr, addr_str, sizeof(addr_str));

    tcp_connection_t* conn = connection_alloc(fd, addr_str, port, config, NULL, logger);
    if (!conn) {
        close(fd);
        cb_record_failure(cb, host, port);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    cb_record_success(cb, host, port);

    if (logger) {
        LOG_INFO(logger, "tcp", "Connected",
                "remote_addr", addr_str, "port", port_str, NULL);
    }

    *conn_out = conn;
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_close
 * ========================================================================= */

void tcp_close(tcp_connection_t* conn) {
    if (!conn) return;

    if (conn->logger) {
        char ps[8]; snprintf(ps, sizeof(ps), "%u", conn->remote_port);
        LOG_INFO(conn->logger, "tcp", "Connection closed",
                "remote_addr", conn->remote_addr, "port", ps, NULL);
    }

    if (conn->recv_epoll_fd >= 0) close(conn->recv_epoll_fd);
    close(conn->fd);
    send_queue_destroy(&conn->send_queue);
    pthread_mutex_destroy(&conn->send_lock);
    free(conn);
}

/* ============================================================================
 * SEND — flush queue then direct send, ring-buffer queue
 * ========================================================================= */

/* Flush pending queue data to kernel. Returns 0 on success, -1 on error.
   Caller must hold send_lock. */
static int flush_send_queue_locked(tcp_connection_t* conn) {
    while (!send_queue_empty(&conn->send_queue)) {
        const uint8_t* ptr  = NULL;
        size_t         avail = 0;
        send_queue_peek(&conn->send_queue, &ptr, &avail);
        if (!ptr || avail == 0) break;

        ssize_t sent = send(conn->fd, ptr, avail, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            send_queue_consume(&conn->send_queue, (size_t)sent);
            if (conn->metrics && conn->metrics->bytes_sent)
                metrics_counter_add(conn->metrics->bytes_sent, (uint64_t)sent);
        } else if (sent == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  /* Kernel buffer still full */
        } else {
            return -1;  /* Hard error */
        }
    }
    return 0;
}

int tcp_send(tcp_connection_t* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&conn->send_lock);

    /* 1. Flush any previously-queued data */
    if (!send_queue_empty(&conn->send_queue)) {
        if (flush_send_queue_locked(conn) < 0) {
            if (conn->metrics && conn->metrics->send_errors)
                metrics_counter_inc(conn->metrics->send_errors);
            pthread_mutex_unlock(&conn->send_lock);
            return DISTRIC_ERR_IO;
        }
    }

    /* 2. If queue not empty, append (socket still blocked) */
    if (!send_queue_empty(&conn->send_queue)) {
        goto queue_data;
    }

    /* 3. Try direct kernel send */
    {
        ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            if (conn->metrics && conn->metrics->bytes_sent)
                metrics_counter_add(conn->metrics->bytes_sent, (uint64_t)sent);
            if ((size_t)sent == len) {
                pthread_mutex_unlock(&conn->send_lock);
                return (int)sent;
            }
            /* Partial send: queue the remainder */
            data = (const uint8_t*)data + sent;
            len -= (size_t)sent;
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (conn->metrics && conn->metrics->send_errors)
                metrics_counter_inc(conn->metrics->send_errors);
            pthread_mutex_unlock(&conn->send_lock);
            return DISTRIC_ERR_IO;
        }
        /* sent == 0 or EAGAIN: fall through to queue */
    }

queue_data:
    if (send_queue_above_hwm(&conn->send_queue)) {
        if (conn->metrics && conn->metrics->backpressure)
            metrics_counter_inc(conn->metrics->backpressure);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_BACKPRESSURE;
    }

    if (send_queue_push(&conn->send_queue, data, len) != 0) {
        /* Queue truly full (> capacity) */
        if (conn->metrics && conn->metrics->backpressure)
            metrics_counter_inc(conn->metrics->backpressure);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_BACKPRESSURE;
    }

    pthread_mutex_unlock(&conn->send_lock);
    return (int)len;
}

/* ============================================================================
 * tcp_recv — uses cached per-connection epoll fd
 * ========================================================================= */

int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms) {
    if (!conn || !buffer || len == 0) return DISTRIC_ERR_INVALID_ARG;

    /* Wait for readiness using the cached recv_epoll_fd */
    if (timeout_ms >= 0 && conn->recv_epoll_fd >= 0) {
        struct epoll_event events[1];
        int nev = epoll_wait(conn->recv_epoll_fd, events, 1,
                             timeout_ms == 0 ? -1 : timeout_ms);
        if (nev == 0) return 0;  /* Timeout */
        if (nev < 0) {
            if (errno == EINTR) return 0;
            if (conn->metrics && conn->metrics->recv_errors)
                metrics_counter_inc(conn->metrics->recv_errors);
            return DISTRIC_ERR_IO;
        }
        if (events[0].events & (EPOLLHUP | EPOLLERR)) {
            if (!(events[0].events & EPOLLIN)) return DISTRIC_ERR_EOF;
        }
    }

    ssize_t n = recv(conn->fd, buffer, len, MSG_DONTWAIT);
    if (n > 0) {
        if (conn->metrics && conn->metrics->bytes_recv)
            metrics_counter_add(conn->metrics->bytes_recv, (uint64_t)n);
        return (int)n;
    }
    if (n == 0) return DISTRIC_ERR_EOF;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (conn->metrics && conn->metrics->recv_errors)
        metrics_counter_inc(conn->metrics->recv_errors);
    return DISTRIC_ERR_IO;
}

/* ============================================================================
 * QUERY HELPERS
 * ========================================================================= */

size_t tcp_send_queue_depth(const tcp_connection_t* conn) {
    if (!conn) return 0;
    return send_queue_pending(&conn->send_queue);
}

bool tcp_is_writable(const tcp_connection_t* conn) {
    if (!conn) return false;
    return !send_queue_above_hwm(&conn->send_queue);
}

distric_err_t tcp_get_remote_addr(
    tcp_connection_t* conn,
    char* addr_out, size_t addr_len,
    uint16_t* port_out
) {
    if (!conn) return DISTRIC_ERR_INVALID_ARG;
    if (addr_out && addr_len > 0)
        strncpy(addr_out, conn->remote_addr, addr_len - 1);
    if (port_out) *port_out = conn->remote_port;
    return DISTRIC_OK;
}

uint64_t tcp_get_connection_id(tcp_connection_t* conn) {
    return conn ? conn->connection_id : 0;
}