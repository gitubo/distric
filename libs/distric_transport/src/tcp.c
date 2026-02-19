/**
 * @file tcp.c
 * @brief DistriC TCP Transport — v4
 *
 * Changes from v3:
 *
 * 1. ACCEPT LOOP BACKPRESSURE (Item 1)
 *    When the work queue reaches TCP_QUEUE_PAUSE_PCT% saturation, the accept
 *    thread disables EPOLLIN on the listen fd via epoll_ctl(MOD). This stops
 *    new SYN processing at the epoll layer — the kernel SYN backlog absorbs
 *    bursts without CPU waste. The accept fd is re-enabled when saturation
 *    drops below TCP_QUEUE_RESUME_PCT%. A new accept_rejections_total metric
 *    is exported.
 *
 * 2. DETERMINISTIC SHUTDOWN (Item 4)
 *    Replaced nanosleep polling with a pthread_cond_timedwait drain barrier.
 *    Workers signal drain_cond when active_connections reaches 0. On timeout,
 *    wq_force_stop() is called: workers immediately exit their pop loop, then
 *    remaining queued connections are force-closed by the shutdown path.
 *    All worker threads are joined before returning from tcp_server_stop().
 *
 * 3. ATOMIC OBSERVABILITY POINTERS (Item 5)
 *    logger and metrics in tcp_server_s are now _Atomic pointers. Shutdown
 *    nullifies them with an atomic_store memory_order_seq_cst barrier.
 *    All hot-path readers use atomic_load_explicit(memory_order_relaxed) which
 *    is safe: NULL-check after load is always coherent; logging a stale non-NULL
 *    pointer is prevented by the seq_cst store before worker join.
 *
 * 4. CACHE-LINE PADDED WORK QUEUE (Item 6)
 *    head (consumer) and tail (producer) are placed on separate 64-byte cache
 *    lines with explicit padding. Eliminates false sharing between the accept
 *    thread (producer) and the worker threads (consumers).
 *
 * 5. DEPRECATE TIMEOUT-BASED RECV (Item 2)
 *    tcp_recv(timeout_ms) is kept for API compatibility but marked deprecated.
 *    Callers should use timeout_ms = -1 (non-blocking) and manage readiness
 *    externally via tcp_is_readable() or their own epoll loop.
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
#include <time.h>

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

/*
 * Accept backpressure thresholds (as percentage of queue capacity).
 * When fill % >= PAUSE: disable EPOLLIN on listen fd.
 * When fill % <  RESUME: re-enable EPOLLIN on listen fd.
 */
#define TCP_QUEUE_PAUSE_PCT           90u
#define TCP_QUEUE_RESUME_PCT          70u

/* Cache line size — used for work queue false-sharing prevention. */
#define CACHE_LINE_BYTES              64u

/* ============================================================================
 * GLOBAL CONNECTION ID COUNTER
 * ========================================================================= */

static _Atomic uint64_t g_conn_id_counter = 1;

/* ============================================================================
 * MODULE-LEVEL CIRCUIT BREAKER
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
    int               recv_epoll_fd;
    char              remote_addr[64];
    uint16_t          remote_port;
    uint64_t          connection_id;

    pthread_mutex_t   send_lock;
    send_queue_t      send_queue;

    const tcp_conn_metrics_t* metrics;
    logger_t*                 logger;
};

/* ============================================================================
 * WORKER POOL STRUCTURES
 *
 * Work queue design: bounded MPSC ring buffer.
 *
 * False-sharing prevention:
 *   head (consumer written) and tail (producer written) are on separate
 *   64-byte cache lines. The accept thread (producer) and worker threads
 *   (consumers) never compete for the same cache line.
 * ========================================================================= */

typedef struct {
    tcp_connection_t*         conn;
    tcp_connection_callback_t callback;
    void*                     userdata;
} work_item_t;

/*
 * Cache-line padded work queue.
 * Layout:
 *   [metadata cache line] items, capacity, shutdown, force_stop
 *   [consumer cache line] head  (+padding)
 *   [producer cache line] tail  (+padding)
 *   [sync fields]         push_lock, not_empty, wait_lock
 */
typedef struct {
    /* ---- metadata -------------------------------------------------------- */
    work_item_t*    items;
    uint32_t        capacity;
    _Atomic bool    shutdown;
    _Atomic bool    force_stop;
    /* pad metadata to 64 bytes */
    char            _pad_meta[CACHE_LINE_BYTES
                               - sizeof(work_item_t*)
                               - sizeof(uint32_t)
                               - 2 * sizeof(bool)];

    /* ---- consumer (workers read/write head) ---- */
    _Atomic uint32_t head;
    char             _pad_cons[CACHE_LINE_BYTES - sizeof(_Atomic uint32_t)];

    /* ---- producer (accept thread writes tail) ---- */
    _Atomic uint32_t tail;
    char             _pad_prod[CACHE_LINE_BYTES - sizeof(_Atomic uint32_t)];

    /* ---- synchronisation ------------------------------------------------- */
    pthread_mutex_t  push_lock;   /* serialise producers */
    pthread_cond_t   not_empty;
    pthread_mutex_t  wait_lock;
} work_queue_t;

typedef struct {
    pthread_t        thread;
    work_queue_t*    queue;
    _Atomic int64_t* active_connections;
    pthread_cond_t*  drain_cond;
    pthread_mutex_t* drain_mutex;
} worker_t;

/* ============================================================================
 * TCP SERVER STRUCTURE
 * ========================================================================= */

struct tcp_server_s {
    int               listen_fd;
    int               epoll_fd;
    char              bind_addr[64];
    uint16_t          port;

    _Atomic int       state;
    _Atomic int64_t   active_connections;

    /* Accept backpressure — set when queue saturation >= PAUSE_PCT */
    _Atomic bool      accept_paused;

    pthread_t         accept_thread;

    tcp_connection_callback_t callback;
    void*                     userdata;

    /* Worker pool */
    worker_t*         workers;
    uint32_t          worker_count;
    work_queue_t      work_queue;

    /*
     * Drain synchronisation.
     * Workers signal drain_cond when active_connections reaches 0.
     * tcp_server_stop() waits on it with a bounded deadline.
     */
    pthread_cond_t    drain_cond;
    pthread_mutex_t   drain_mutex;

    /*
     * Observability — stored as atomic pointers.
     *
     * Lifecycle contract:
     *   - Set (non-NULL) during tcp_server_create_with_config().
     *   - Nullified with atomic_store(seq_cst) in tcp_server_stop() AFTER
     *     the accept thread is joined but BEFORE workers are joined.
     *   - All readers (accept thread, workers) use relaxed atomic load +
     *     NULL guard. The seq_cst store provides a full memory barrier,
     *     ensuring workers either see the NULL or a valid pointer.
     *   - Workers cannot call any obs function after the join returns.
     */
    _Atomic(metrics_registry_t*) metrics;
    _Atomic(logger_t*)           logger;

    metric_t*   connections_total_metric;
    metric_t*   active_connections_metric;
    metric_t*   accept_errors_metric;
    metric_t*   queue_full_metric;
    metric_t*   accept_rejections_metric;  /* NEW: reject due to backpressure */

    tcp_conn_metrics_t conn_metrics;

    tcp_server_config_t     config;
    tcp_connection_config_t conn_config;

    uint32_t drain_timeout_ms;
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

/*
 * Constrain the kernel send buffer to match the configured send-queue capacity.
 * This ensures EAGAIN fires at roughly the same threshold as the in-process
 * HWM, so backpressure triggers as designed. Without this, the kernel auto-tuned
 * loopback buffer (often 4+ MB) absorbs all data before the queue fills,
 * rendering HWM-based backpressure inert.
 */
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
 * WORK QUEUE
 * ========================================================================= */

static int wq_init(work_queue_t* q, uint32_t capacity) {
    q->items    = calloc(capacity, sizeof(work_item_t));
    if (!q->items) return -1;
    q->capacity = capacity;
    atomic_init(&q->head,       0);
    atomic_init(&q->tail,       0);
    atomic_init(&q->shutdown,   false);
    atomic_init(&q->force_stop, false);
    pthread_mutex_init(&q->push_lock, NULL);
    pthread_mutex_init(&q->wait_lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

/* Returns approximate fill count (relaxed, best-effort). */
static uint32_t wq_used(const work_queue_t* q) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    return (t >= h) ? (t - h) : (t + q->capacity - h);
}

/* Normal shutdown: signal workers to drain queue then exit. */
static void wq_shutdown(work_queue_t* q) {
    pthread_mutex_lock(&q->wait_lock);
    atomic_store(&q->shutdown, true);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
}

/*
 * Force shutdown: workers immediately exit even if queue has items.
 * Remaining items are drained by the caller after joining workers.
 */
static void wq_force_stop(work_queue_t* q) {
    pthread_mutex_lock(&q->wait_lock);
    atomic_store(&q->force_stop, true);
    atomic_store(&q->shutdown,   true);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
}

/* Returns 0 on success, -1 if queue full. */
static int wq_push(work_queue_t* q, const work_item_t* item) {
    pthread_mutex_lock(&q->push_lock);
    uint32_t h    = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t    = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t next = (t + 1) % q->capacity;
    if (next == h) {
        pthread_mutex_unlock(&q->push_lock);
        return -1;
    }
    q->items[t] = *item;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    pthread_mutex_unlock(&q->push_lock);

    pthread_mutex_lock(&q->wait_lock);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
    return 0;
}

/*
 * Pop a single item without blocking. Returns 0 on success, -1 if empty.
 * Used during shutdown draining (no workers running).
 */
static int wq_try_pop(work_queue_t* q, work_item_t* out) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (h == t) return -1;
    *out = q->items[h];
    atomic_store_explicit(&q->head, (h + 1) % q->capacity, memory_order_release);
    return 0;
}

/*
 * Blocking pop. Returns 0 on success, -1 on shutdown/force_stop.
 * force_stop: exit immediately even if items remain.
 * shutdown:   exit only when queue is empty.
 */
static int wq_pop(work_queue_t* q, work_item_t* out) {
    pthread_mutex_lock(&q->wait_lock);
    while (true) {
        /* Force-stop: exit unconditionally, leave remaining items for drain. */
        if (atomic_load_explicit(&q->force_stop, memory_order_relaxed)) {
            pthread_mutex_unlock(&q->wait_lock);
            return -1;
        }

        uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
        uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
        if (h != t) {
            *out = q->items[h];
            atomic_store_explicit(&q->head, (h + 1) % q->capacity,
                                  memory_order_release);
            pthread_mutex_unlock(&q->wait_lock);
            return 0;
        }

        /* Queue empty and graceful shutdown: exit. */
        if (atomic_load_explicit(&q->shutdown, memory_order_relaxed)) {
            pthread_mutex_unlock(&q->wait_lock);
            return -1;
        }

        pthread_cond_wait(&q->not_empty, &q->wait_lock);
    }
}

static void wq_destroy(work_queue_t* q) {
    pthread_mutex_destroy(&q->push_lock);
    pthread_mutex_destroy(&q->wait_lock);
    pthread_cond_destroy(&q->not_empty);
    free(q->items);
    q->items = NULL;
}

/* ============================================================================
 * WORKER THREAD
 * ========================================================================= */

static void* worker_thread_func(void* arg) {
    worker_t* w = (worker_t*)arg;

    work_item_t item;
    while (wq_pop(w->queue, &item) == 0) {
        item.callback(item.conn, item.userdata);

        int64_t remaining = atomic_fetch_add(w->active_connections, -1) - 1;
        if (remaining == 0) {
            /* Signal drain barrier — shutdown may be waiting. */
            pthread_mutex_lock(w->drain_mutex);
            pthread_cond_signal(w->drain_cond);
            pthread_mutex_unlock(w->drain_mutex);
        }
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

    /* Align SO_SNDBUF with the in-process queue capacity so the kernel
     * buffer fills at the same rate as our queue. This makes EAGAIN fire
     * before the queue is bypassed, keeping backpressure effective.
     * Only applied when the caller explicitly sized the queue. */
    if (cfg && cfg->send_queue_capacity)
        set_sndbuf(fd, q_cap);

    conn->recv_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (conn->recv_epoll_fd >= 0) {
        struct epoll_event ev = {
            .events  = TCP_RECV_EPOLL_FLAGS,
            .data.fd = fd
        };
        epoll_ctl(conn->recv_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    pthread_mutex_init(&conn->send_lock, NULL);

    conn->fd            = fd;
    conn->remote_port   = remote_port;
    conn->connection_id = atomic_fetch_add(&g_conn_id_counter, 1);
    conn->metrics       = shared_metrics;
    conn->logger        = logger;

    strncpy(conn->remote_addr,
            remote_addr ? remote_addr : "unknown",
            sizeof(conn->remote_addr) - 1);

    return conn;
}

/* ============================================================================
 * tcp_server_create / tcp_server_create_with_config
 * ========================================================================= */

distric_err_t tcp_server_create_with_config(
    const char*                bind_addr,
    uint16_t                   port,
    const tcp_server_config_t* cfg,
    metrics_registry_t*        metrics,
    logger_t*                  logger,
    tcp_server_t**             server_out
) {
    if (!bind_addr || !server_out) return DISTRIC_ERR_INVALID_ARG;

    tcp_server_t* srv = calloc(1, sizeof(*srv));
    if (!srv) return DISTRIC_ERR_ALLOC_FAILURE;

    srv->worker_count     = cfg && cfg->worker_threads   ? cfg->worker_threads   : cpu_count();
    if (srv->worker_count > TCP_MAX_WORKERS) srv->worker_count = TCP_MAX_WORKERS;
    uint32_t queue_depth  = cfg && cfg->worker_queue_depth ? cfg->worker_queue_depth : TCP_DEFAULT_QUEUE_DEPTH;
    srv->drain_timeout_ms = cfg && cfg->drain_timeout_ms   ? cfg->drain_timeout_ms   : TCP_DEFAULT_DRAIN_TIMEOUT_MS;

    srv->conn_config.send_queue_capacity = cfg ? cfg->conn_send_queue_capacity : 0;
    srv->conn_config.send_queue_hwm      = cfg ? cfg->conn_send_queue_hwm      : 0;

    atomic_store(&srv->metrics, metrics);
    atomic_store(&srv->logger,  logger);
    srv->port = port;
    strncpy(srv->bind_addr, bind_addr, sizeof(srv->bind_addr) - 1);

    atomic_init(&srv->state,              TCP_SERVER_STOPPED);
    atomic_init(&srv->active_connections, 0);
    atomic_init(&srv->accept_paused,      false);

    pthread_cond_init(&srv->drain_cond, NULL);
    pthread_mutex_init(&srv->drain_mutex, NULL);

    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) { free(srv); return DISTRIC_ERR_INIT_FAILED; }

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

    /* Register server-level metrics once. */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_server_connections_total",
            "Total accepted connections", NULL, 0, &srv->connections_total_metric);
        metrics_register_gauge(metrics, "tcp_server_active_connections",
            "Currently active connections", NULL, 0, &srv->active_connections_metric);
        metrics_register_counter(metrics, "tcp_server_accept_errors_total",
            "Total accept errors", NULL, 0, &srv->accept_errors_metric);
        metrics_register_counter(metrics, "tcp_server_queue_full_total",
            "Connections rejected: worker queue full", NULL, 0, &srv->queue_full_metric);
        metrics_register_counter(metrics, "tcp_server_accept_rejections_total",
            "Connections rejected due to accept backpressure (queue saturated)",
            NULL, 0, &srv->accept_rejections_metric);

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

    if (wq_init(&srv->work_queue, queue_depth) != 0) {
        close(srv->listen_fd); close(srv->epoll_fd); free(srv);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

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
                "bind_addr", bind_addr, "port", ps, "workers", ws, NULL);
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
 *
 * Backpressure logic:
 *   1. After each epoll_wait, check if accept is paused.
 *      - If paused: poll queue fill; re-enable EPOLLIN if fill < RESUME_PCT.
 *   2. After a successful connection enqueue, if the queue fill exceeds
 *      PAUSE_PCT, disable EPOLLIN on the listen fd and set accept_paused.
 *   3. When accept is paused, epoll_wait returns only on the timeout, so the
 *      thread burns minimal CPU while waiting for queue to drain.
 * ========================================================================= */

static void accept_pause(tcp_server_t* srv) {
    if (!atomic_load_explicit(&srv->accept_paused, memory_order_relaxed)) {
        struct epoll_event ev = { .events = 0, .data.fd = srv->listen_fd };
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_MOD, srv->listen_fd, &ev);
        atomic_store_explicit(&srv->accept_paused, true, memory_order_relaxed);

        logger_t* log = atomic_load_explicit(&srv->logger, memory_order_relaxed);
        if (log) {
            LOG_WARN(log, "tcp_server", "Accept paused: queue saturated", NULL);
        }
    }
}

static void accept_resume(tcp_server_t* srv) {
    if (atomic_load_explicit(&srv->accept_paused, memory_order_relaxed)) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_MOD, srv->listen_fd, &ev);
        atomic_store_explicit(&srv->accept_paused, false, memory_order_relaxed);

        logger_t* log = atomic_load_explicit(&srv->logger, memory_order_relaxed);
        if (log) {
            LOG_INFO(log, "tcp_server", "Accept resumed: queue below threshold", NULL);
        }
    }
}

static void* accept_thread_func(void* arg) {
    tcp_server_t* srv = (tcp_server_t*)arg;
    struct epoll_event events[TCP_EPOLL_MAX_EVENTS];

    while (atomic_load(&srv->state) == TCP_SERVER_RUNNING) {
        int n = epoll_wait(srv->epoll_fd, events, TCP_EPOLL_MAX_EVENTS,
                           TCP_ACCEPT_TIMEOUT_MS);

        /*
         * On each wake-up (event or timeout) check if accept should be
         * re-enabled. This fires at most every TCP_ACCEPT_TIMEOUT_MS ms
         * when paused — acceptable poll interval for queue draining.
         */
        if (atomic_load_explicit(&srv->accept_paused, memory_order_relaxed)) {
            uint32_t used = wq_used(&srv->work_queue);
            uint32_t pct  = (srv->work_queue.capacity > 0)
                            ? (used * 100u / srv->work_queue.capacity) : 0u;
            if (pct < TCP_QUEUE_RESUME_PCT) {
                accept_resume(srv);
            }
        }

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
                logger_t* log = atomic_load_explicit(&srv->logger, memory_order_relaxed);
                if (log) {
                    LOG_ERROR(log, "tcp_server", "Accept failed",
                             "error", transport_err_str(transport_classify_errno(errno)), NULL);
                }
                continue;
            }

            set_nonblocking(cfd);
            set_tcp_nodelay(cfd);

            char addr_str[64];
            inet_ntop(AF_INET, &caddr.sin_addr, addr_str, sizeof(addr_str));
            uint16_t rport = ntohs(caddr.sin_port);

            /* Check saturation before allocation to avoid work when overloaded. */
            uint32_t used = wq_used(&srv->work_queue);
            uint32_t pct  = (srv->work_queue.capacity > 0)
                            ? (used * 100u / srv->work_queue.capacity) : 0u;
            if (pct >= TCP_QUEUE_PAUSE_PCT) {
                close(cfd);
                if (srv->accept_rejections_metric)
                    metrics_counter_inc(srv->accept_rejections_metric);
                accept_pause(srv);
                continue;
            }

            metrics_registry_t* m   = atomic_load_explicit(&srv->metrics, memory_order_relaxed);
            logger_t*           log = atomic_load_explicit(&srv->logger,  memory_order_relaxed);

            tcp_connection_t* conn = connection_alloc(
                cfd, addr_str, rport,
                &srv->conn_config,
                m ? &srv->conn_metrics : NULL,
                log);

            if (!conn) { close(cfd); continue; }

            work_item_t item = {
                .conn     = conn,
                .callback = srv->callback,
                .userdata = srv->userdata,
            };

            if (wq_push(&srv->work_queue, &item) != 0) {
                /* Queue full at push time (race with other producers impossible
                 * — only one accept thread — but defensive check). */
                if (srv->queue_full_metric)
                    metrics_counter_inc(srv->queue_full_metric);
                if (log) {
                    LOG_WARN(log, "tcp_server",
                            "Worker queue full, connection rejected",
                            "remote_addr", addr_str, NULL);
                }
                tcp_close(conn);

                /* Pause accept: queue is saturated. */
                accept_pause(srv);
                continue;
            }

            int64_t active = atomic_fetch_add(&srv->active_connections, 1) + 1;
            if (srv->connections_total_metric)
                metrics_counter_inc(srv->connections_total_metric);
            if (srv->active_connections_metric)
                metrics_gauge_set(srv->active_connections_metric, (double)active);

            if (log) {
                char ps[8]; snprintf(ps, sizeof(ps), "%u", rport);
                LOG_INFO(log, "tcp_server", "Connection accepted",
                        "remote_addr", addr_str, "remote_port", ps, NULL);
            }

            /* Check saturation again after push and pause if needed. */
            used = wq_used(&srv->work_queue);
            pct  = (srv->work_queue.capacity > 0)
                   ? (used * 100u / srv->work_queue.capacity) : 0u;
            if (pct >= TCP_QUEUE_PAUSE_PCT) {
                accept_pause(srv);
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
    if (atomic_load(&server->state) != TCP_SERVER_STOPPED)
        return DISTRIC_ERR_INVALID_STATE;

    server->callback = on_connection;
    server->userdata = userdata;
    atomic_store(&server->state, TCP_SERVER_RUNNING);

    for (uint32_t i = 0; i < server->worker_count; i++) {
        server->workers[i].queue              = &server->work_queue;
        server->workers[i].active_connections = &server->active_connections;
        server->workers[i].drain_cond         = &server->drain_cond;
        server->workers[i].drain_mutex        = &server->drain_mutex;
        pthread_create(&server->workers[i].thread, NULL,
                       worker_thread_func, &server->workers[i]);
    }

    pthread_create(&server->accept_thread, NULL, accept_thread_func, server);
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_server_stop — Deterministic shutdown
 *
 * Phase 1: Stop accepting.  Set state → DRAINING, join accept thread.
 * Phase 2: Drain.  Wait on drain_cond up to drain_timeout_ms for
 *          active_connections to reach 0.
 * Phase 3: Enforce.  If timeout, call wq_force_stop(), join workers,
 *          then drain and close any remaining queued connections.
 *          If clean drain, call wq_shutdown(), join workers normally.
 * Phase 4: Null observability pointers with seq_cst barrier.
 * ========================================================================= */

distric_err_t tcp_server_stop(tcp_server_t* server) {
    if (!server) return DISTRIC_ERR_INVALID_ARG;

    tcp_server_state_t prev = (tcp_server_state_t)atomic_exchange(
        &server->state, TCP_SERVER_DRAINING);
    if (prev == TCP_SERVER_STOPPED) return DISTRIC_OK;
    if (prev == TCP_SERVER_DRAINING) return DISTRIC_OK;

    logger_t* log = atomic_load_explicit(&server->logger, memory_order_relaxed);
    if (log) {
        LOG_INFO(log, "tcp_server", "Draining server", NULL);
    }

    /* Phase 1: stop the accept thread. */
    pthread_join(server->accept_thread, NULL);

    /* Phase 2: wait for active connections to drain. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint32_t to_ms = server->drain_timeout_ms;
    deadline.tv_sec  += (long)(to_ms / 1000u);
    deadline.tv_nsec += (long)(to_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    bool clean_drain = false;
    pthread_mutex_lock(&server->drain_mutex);
    while (atomic_load(&server->active_connections) > 0) {
        int rc = pthread_cond_timedwait(&server->drain_cond,
                                        &server->drain_mutex, &deadline);
        if (rc == ETIMEDOUT) break;
    }
    clean_drain = (atomic_load(&server->active_connections) == 0);
    pthread_mutex_unlock(&server->drain_mutex);

    /* Phase 3: enforce termination. */
    if (!clean_drain) {
        int64_t remaining = atomic_load(&server->active_connections);
        log = atomic_load_explicit(&server->logger, memory_order_relaxed);
        if (remaining > 0 && log) {
            char rbuf[24]; snprintf(rbuf, sizeof(rbuf), "%lld", (long long)remaining);
            LOG_WARN(log, "tcp_server",
                    "Drain timeout: force-stopping worker queue",
                    "remaining", rbuf, NULL);
        }

        /* Signal workers to exit unconditionally. */
        wq_force_stop(&server->work_queue);
    } else {
        wq_shutdown(&server->work_queue);
    }

    /* Join all workers. After this, no worker can access the queue. */
    for (uint32_t i = 0; i < server->worker_count; i++) {
        pthread_join(server->workers[i].thread, NULL);
    }

    /*
     * If force-stopped: close any items remaining in the queue.
     * Workers have exited; single-threaded access is safe here.
     */
    if (!clean_drain) {
        work_item_t item;
        while (wq_try_pop(&server->work_queue, &item) == 0) {
            tcp_close(item.conn);
            atomic_fetch_add(&server->active_connections, -1);
        }
    }

    wq_destroy(&server->work_queue);

    /*
     * Phase 4: nullify observability with seq_cst barrier.
     * Any worker that loaded a non-NULL pointer before this store has already
     * finished (joined above). No worker can dereference after the join.
     */
    atomic_store_explicit(&server->logger,  (logger_t*)NULL,           memory_order_seq_cst);
    atomic_store_explicit(&server->metrics, (metrics_registry_t*)NULL, memory_order_seq_cst);

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
    pthread_cond_destroy(&server->drain_cond);
    pthread_mutex_destroy(&server->drain_mutex);
    close(server->listen_fd);
    close(server->epoll_fd);
    free(server->workers);
    free(server);
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
    (void)metrics; /* Standalone connections share no server metrics. */
    if (!host || !conn_out) return DISTRIC_ERR_INVALID_ARG;

    cb_registry_t* cb = get_cb_registry();
    if (!cb_is_allowed(cb, host, port)) {
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
    /* SO_SNDBUF: align kernel buffer with in-process queue capacity */
    if (config && config->send_queue_capacity)
        set_sndbuf(fd, config->send_queue_capacity);

    struct sockaddr_in saved_addr;
    memcpy(&saved_addr, result->ai_addr,
           result->ai_addrlen < sizeof(saved_addr)
           ? result->ai_addrlen : sizeof(saved_addr));
    freeaddrinfo(result);

    int cr = connect(fd, (struct sockaddr*)&saved_addr, sizeof(saved_addr));

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
 * SEND PATH
 * ========================================================================= */

static int flush_send_queue_locked(tcp_connection_t* conn) {
    while (!send_queue_empty(&conn->send_queue)) {
        const uint8_t* ptr   = NULL;
        size_t         avail = 0;
        send_queue_peek(&conn->send_queue, &ptr, &avail);
        if (!ptr || avail == 0) break;

        ssize_t sent = send(conn->fd, ptr, avail, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            send_queue_consume(&conn->send_queue, (size_t)sent);
            if (conn->metrics && conn->metrics->bytes_sent)
                metrics_counter_add(conn->metrics->bytes_sent, (uint64_t)sent);
        } else if (sent == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return -1;
        }
    }
    return 0;
}

int tcp_send(tcp_connection_t* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&conn->send_lock);

    if (!send_queue_empty(&conn->send_queue)) {
        if (flush_send_queue_locked(conn) < 0) {
            if (conn->metrics && conn->metrics->send_errors)
                metrics_counter_inc(conn->metrics->send_errors);
            pthread_mutex_unlock(&conn->send_lock);
            return DISTRIC_ERR_IO;
        }
    }

    if (!send_queue_empty(&conn->send_queue)) {
        goto queue_data;
    }

    {
        ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            if (conn->metrics && conn->metrics->bytes_sent)
                metrics_counter_add(conn->metrics->bytes_sent, (uint64_t)sent);
            if ((size_t)sent == len) {
                pthread_mutex_unlock(&conn->send_lock);
                return (int)sent;
            }
            data = (const uint8_t*)data + sent;
            len -= (size_t)sent;
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (conn->metrics && conn->metrics->send_errors)
                metrics_counter_inc(conn->metrics->send_errors);
            pthread_mutex_unlock(&conn->send_lock);
            return DISTRIC_ERR_IO;
        }
    }

queue_data:
    if (send_queue_above_hwm(&conn->send_queue)) {
        if (conn->metrics && conn->metrics->backpressure)
            metrics_counter_inc(conn->metrics->backpressure);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_BACKPRESSURE;
    }

    if (send_queue_push(&conn->send_queue, data, len) != 0) {
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
 *
 * DEPRECATED: timeout_ms >= 0 blocks inside this function.
 * Callers should migrate to: timeout_ms = -1 + tcp_is_readable() for
 * readiness checks from their own event loop.
 * ========================================================================= */

int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms) {
    if (!conn || !buffer || len == 0) return DISTRIC_ERR_INVALID_ARG;

    if (timeout_ms >= 0 && conn->recv_epoll_fd >= 0) {
        struct epoll_event events[1];
        int nev = epoll_wait(conn->recv_epoll_fd, events, 1,
                             timeout_ms == 0 ? -1 : timeout_ms);
        if (nev == 0) return 0;
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

/*
 * tcp_is_readable — event-driven readiness check.
 *
 * Polls the cached recv_epoll_fd with zero timeout. Returns true if
 * the connection has data ready to read (EPOLLIN set). Callers should
 * use this instead of passing timeout_ms >= 0 to tcp_recv().
 */
bool tcp_is_readable(const tcp_connection_t* conn) {
    if (!conn || conn->recv_epoll_fd < 0) return false;
    struct epoll_event ev;
    int n = epoll_wait(conn->recv_epoll_fd, &ev, 1, 0);
    return (n > 0) && (ev.events & EPOLLIN);
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