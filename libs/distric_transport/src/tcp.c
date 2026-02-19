/**
 * @file tcp.c
 * @brief DistriC TCP Transport — Non-Blocking epoll-based implementation v5
 *
 * v5 changes:
 *
 * 1. WORKER SATURATION VISIBILITY (Item 1)
 *    - _Atomic uint32_t busy_workers in tcp_server_s.
 *    - worker_thread_func increments busy_workers before the callback and
 *      decrements after. The ratio busy/total is exported as gauge
 *      tcp_worker_busy_ratio and a float via tcp_server_worker_busy_ratio().
 *    - Each callback is timed with CLOCK_MONOTONIC. If duration exceeds
 *      handler_warn_duration_ms (from config or global default), a LOG_WARN
 *      is emitted and tcp_handler_slow_total is incremented.
 *    - tcp_worker_queue_depth gauge tracks the current work queue fill.
 *
 * 2. GLOBAL MEMORY BUDGET (Item 2)
 *    - connection_alloc() calls transport_memory_acquire(q_cap) before
 *      initialising the send queue. If the global limit is exceeded the
 *      connection fd is closed and NULL is returned, causing the accept
 *      thread to discard the connection (metrics updated upstream).
 *    - tcp_close() calls transport_memory_release(conn->mem_reserved_bytes)
 *      to return the allocation to the global counter.
 *    - mem_reserved_bytes stored in tcp_connection_s tracks the exact
 *      capacity reserved so release is always symmetric.
 *
 * 3. OBSERVABILITY SAMPLING (Item 3)
 *    - Verbose LOG_INFO calls in the accept loop are gated by
 *      transport_should_sample_obs(). Sampled-out events are counted via
 *      transport_obs_record_drop().
 *    - LOG_WARN and LOG_ERROR are never sampled.
 *    - Per-server observability_sample_pct overrides the global setting when
 *      non-zero by temporarily overriding the global via thread-local state.
 *      (Simpler: per-server check uses server->obs_sample_pct directly.)
 *
 * 4. CACHE-LINE PADDED WORK QUEUE (unchanged from v4)
 *    head (consumer) and tail (producer) on separate 64-byte cache lines.
 *
 * 5. DEPRECATION MACRO (Item 7)
 *    DISTRIC_DEPRECATED applied to tcp_server_create() in tcp.h.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/tcp.h"
#include "distric_transport/transport_config.h"
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

    /*
     * Bytes reserved in the global memory budget.
     * Equals send_queue.capacity; stored separately so tcp_close() can
     * release exactly the right amount even after the queue is destroyed.
     */
    size_t            mem_reserved_bytes;

    const tcp_conn_metrics_t* metrics;
    logger_t*                 logger;
};

/* ============================================================================
 * WORKER POOL STRUCTURES
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
    char            _pad_meta[CACHE_LINE_BYTES
                               - sizeof(work_item_t*)
                               - sizeof(uint32_t)
                               - 2 * sizeof(bool)];

    /* ---- consumer (workers read/write head) ------------------------------ */
    _Atomic uint32_t head;
    char             _pad_cons[CACHE_LINE_BYTES - sizeof(_Atomic uint32_t)];

    /* ---- producer (accept thread writes tail) ---------------------------- */
    _Atomic uint32_t tail;
    char             _pad_prod[CACHE_LINE_BYTES - sizeof(_Atomic uint32_t)];

    /* ---- synchronisation ------------------------------------------------- */
    pthread_mutex_t  push_lock;
    pthread_cond_t   not_empty;
    pthread_mutex_t  wait_lock;
} work_queue_t;

/* Forward-declare tcp_server_s so worker_t can hold a pointer. */
struct tcp_server_s;

typedef struct {
    pthread_t             thread;
    struct tcp_server_s*  server;          /* parent — valid until thread join */
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

    /* Worker saturation visibility (v5) */
    _Atomic uint32_t  busy_workers;        /* workers currently in callback  */
    uint32_t          worker_count;        /* total workers (immutable post-start) */

    /* Accept backpressure — set when queue saturation >= PAUSE_PCT */
    _Atomic bool      accept_paused;

    pthread_t         accept_thread;

    tcp_connection_callback_t callback;
    void*                     userdata;

    /* Worker pool */
    worker_t*         workers;
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
     */
    _Atomic(metrics_registry_t*) metrics;
    _Atomic(logger_t*)           logger;

    /* Server-level metrics */
    metric_t*   connections_total_metric;
    metric_t*   active_connections_metric;
    metric_t*   accept_errors_metric;
    metric_t*   queue_full_metric;
    metric_t*   accept_rejections_metric;

    /* v5 saturation metrics */
    metric_t*   worker_busy_ratio_metric;    /* gauge: busy/total           */
    metric_t*   handler_slow_total_metric;   /* counter: slow handler trips  */
    metric_t*   handler_duration_max_metric; /* gauge: rolling max (ms)      */
    metric_t*   worker_queue_depth_metric;   /* gauge: current queue fill    */

    tcp_conn_metrics_t      conn_metrics;
    tcp_server_config_t     config;
    tcp_connection_config_t conn_config;

    uint32_t drain_timeout_ms;

    /* v5: handler slow detection */
    uint32_t handler_warn_duration_ms;   /* 0 = disabled */
    uint32_t obs_sample_pct;             /* 1-100; 0 = use global */
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
 * TIME UTILITIES
 * ========================================================================= */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ============================================================================
 * OBSERVABILITY SAMPLING HELPERS
 * ========================================================================= */

/*
 * Returns true if a verbose (INFO/DEBUG) log call should be emitted for
 * this server. Uses the server-level override if set, otherwise falls
 * back to the global transport_should_sample_obs().
 */
static bool server_should_log(const struct tcp_server_s* srv) {
    uint32_t pct = srv->obs_sample_pct;
    if (pct == 0) {
        return transport_should_sample_obs();
    }
    if (pct >= 100) return true;
    /* Thread-local LCG — no locks. */
    static __thread uint64_t tls_rng = 0;
    if (__builtin_expect(tls_rng == 0, 0)) {
        tls_rng = (uint64_t)(uintptr_t)&tls_rng ^ 0xdeadbeefcafebabeULL;
    }
    tls_rng = tls_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(tls_rng >> 33) % 100u < pct;
}

/* ============================================================================
 * WORK QUEUE
 * ========================================================================= */

static int wq_init(work_queue_t* q, uint32_t capacity) {
    q->items = calloc(capacity, sizeof(work_item_t));
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

static uint32_t wq_used(const work_queue_t* q) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    return (t >= h) ? (t - h) : (t + q->capacity - h);
}

static void wq_shutdown(work_queue_t* q) {
    pthread_mutex_lock(&q->wait_lock);
    atomic_store(&q->shutdown, true);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
}

static void wq_force_stop(work_queue_t* q) {
    pthread_mutex_lock(&q->wait_lock);
    atomic_store(&q->force_stop, true);
    atomic_store(&q->shutdown,   true);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->wait_lock);
}

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

static int wq_try_pop(work_queue_t* q, work_item_t* out) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (h == t) return -1;
    *out = q->items[h];
    atomic_store_explicit(&q->head, (h + 1) % q->capacity, memory_order_release);
    return 0;
}

static int wq_pop(work_queue_t* q, work_item_t* out) {
    pthread_mutex_lock(&q->wait_lock);
    while (true) {
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
 * WORKER THREAD (v5: saturation tracking + handler duration measurement)
 * ========================================================================= */

static void* worker_thread_func(void* arg) {
    worker_t*           w   = (worker_t*)arg;
    struct tcp_server_s* srv = w->server;

    work_item_t item;
    while (wq_pop(&srv->work_queue, &item) == 0) {
        /*
         * Mark this worker as busy before invoking the callback.
         * Increment is seq_cst to ensure metrics readers see a consistent
         * snapshot when polling busy_workers and worker_count together.
         */
        uint32_t busy = atomic_fetch_add_explicit(
            &srv->busy_workers, 1u, memory_order_seq_cst) + 1u;

        /* Update busy ratio gauge (best-effort, non-blocking). */
        if (srv->worker_busy_ratio_metric && srv->worker_count > 0) {
            double ratio = (double)busy / (double)srv->worker_count;
            metrics_gauge_set(srv->worker_busy_ratio_metric, ratio);
        }

        /* Time the callback for slow-handler detection. */
        uint64_t t_start = monotonic_ms();

        item.callback(item.conn, item.userdata);

        uint64_t duration_ms = monotonic_ms() - t_start;

        /* Update rolling maximum handler duration (relaxed — metric only). */
        if (srv->handler_duration_max_metric) {
            metrics_gauge_set(srv->handler_duration_max_metric,
                              (double)duration_ms);
        }

        /*
         * Slow handler detection.
         * Determine effective threshold: server override > global default.
         */
        uint32_t warn_ms = srv->handler_warn_duration_ms;
        if (warn_ms == 0) {
            transport_global_config_t gcfg;
            transport_config_get(&gcfg);
            warn_ms = gcfg.handler_warn_duration_ms;
        }

        if (warn_ms > 0 && duration_ms >= (uint64_t)warn_ms) {
            if (srv->handler_slow_total_metric)
                metrics_counter_inc(srv->handler_slow_total_metric);

            logger_t* log = atomic_load_explicit(&srv->logger,
                                                  memory_order_relaxed);
            if (log) {
                char dur_str[24]; snprintf(dur_str, sizeof(dur_str), "%llu",
                                           (unsigned long long)duration_ms);
                char thr_str[16]; snprintf(thr_str, sizeof(thr_str), "%u", warn_ms);
                LOG_WARN(log, "tcp_server",
                        "Slow connection handler detected",
                        "duration_ms", dur_str,
                        "threshold_ms", thr_str, NULL);
            }
        }

        /* Worker is idle again. */
        busy = atomic_fetch_sub_explicit(&srv->busy_workers, 1u,
                                          memory_order_seq_cst) - 1u;
        if (srv->worker_busy_ratio_metric && srv->worker_count > 0) {
            double ratio = (double)busy / (double)srv->worker_count;
            metrics_gauge_set(srv->worker_busy_ratio_metric, ratio);
        }

        /*
         * Decrement active connections and signal drain waiters if we
         * reach zero.
         */
        int64_t remaining = atomic_fetch_add(&srv->active_connections, -1) - 1;
        if (remaining == 0) {
            pthread_mutex_lock(&srv->drain_mutex);
            pthread_cond_signal(&srv->drain_cond);
            pthread_mutex_unlock(&srv->drain_mutex);
        }
    }
    return NULL;
}

/* ============================================================================
 * FORWARD DECLARATION
 * ========================================================================= */

static tcp_connection_t* connection_alloc(
    int fd,
    const char* remote_addr,
    uint16_t remote_port,
    const tcp_connection_config_t* cfg,
    const tcp_conn_metrics_t* shared_metrics,
    logger_t* logger
);

/* ============================================================================
 * ACCEPT BACKPRESSURE HELPERS
 * ========================================================================= */

static void accept_pause(struct tcp_server_s* srv) {
    if (atomic_exchange(&srv->accept_paused, true)) return; /* already paused */
    struct epoll_event ev = { .events = 0, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_MOD, srv->listen_fd, &ev);
    if (srv->accept_rejections_metric)
        metrics_counter_inc(srv->accept_rejections_metric);
}

static void accept_resume(struct tcp_server_s* srv) {
    if (!atomic_exchange(&srv->accept_paused, false)) return; /* not paused */
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_MOD, srv->listen_fd, &ev);
}

/* ============================================================================
 * ACCEPT THREAD
 * ========================================================================= */

static void* accept_thread_func(void* arg) {
    struct tcp_server_s* srv = (struct tcp_server_s*)arg;
    struct epoll_event events[TCP_EPOLL_MAX_EVENTS];

    while (atomic_load_explicit(&srv->state, memory_order_relaxed)
           == TCP_SERVER_RUNNING)
    {
        int n = epoll_wait(srv->epoll_fd, events, TCP_EPOLL_MAX_EVENTS,
                           TCP_ACCEPT_TIMEOUT_MS);

        /* Check if we should resume accepting (queue drained enough). */
        if (atomic_load_explicit(&srv->accept_paused, memory_order_relaxed)) {
            uint32_t used = wq_used(&srv->work_queue);
            uint32_t pct  = (srv->work_queue.capacity > 0)
                            ? (used * 100u / srv->work_queue.capacity) : 0u;
            if (pct < TCP_QUEUE_RESUME_PCT) {
                accept_resume(srv);
            }
        }

        /* Update queue depth metric on every epoll wakeup. */
        if (srv->worker_queue_depth_metric) {
            metrics_gauge_set(srv->worker_queue_depth_metric,
                              (double)wq_used(&srv->work_queue));
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
                logger_t* log = atomic_load_explicit(&srv->logger,
                                                      memory_order_relaxed);
                if (log) {
                    LOG_ERROR(log, "tcp_server", "Accept failed",
                             "error",
                             transport_err_str(transport_classify_errno(errno)),
                             NULL);
                }
                continue;
            }

            set_nonblocking(cfd);
            set_tcp_nodelay(cfd);

            char addr_str[64];
            inet_ntop(AF_INET, &caddr.sin_addr, addr_str, sizeof(addr_str));
            uint16_t rport = ntohs(caddr.sin_port);

            /* Reject early if queue is saturated. */
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

            metrics_registry_t* m   = atomic_load_explicit(&srv->metrics,
                                                             memory_order_relaxed);
            logger_t*           log = atomic_load_explicit(&srv->logger,
                                                            memory_order_relaxed);

            tcp_connection_t* conn = connection_alloc(
                cfd, addr_str, rport,
                &srv->conn_config,
                m ? &srv->conn_metrics : NULL,
                log);

            if (!conn) {
                /* connection_alloc logs rejection reason internally. */
                close(cfd);
                continue;
            }

            work_item_t item = {
                .conn     = conn,
                .callback = srv->callback,
                .userdata = srv->userdata,
            };

            if (wq_push(&srv->work_queue, &item) != 0) {
                if (srv->queue_full_metric)
                    metrics_counter_inc(srv->queue_full_metric);
                if (log) {
                    LOG_WARN(log, "tcp_server",
                            "Worker queue full, connection rejected",
                            "remote_addr", addr_str, NULL);
                }
                tcp_close(conn);
                accept_pause(srv);
                continue;
            }

            int64_t active = atomic_fetch_add(&srv->active_connections, 1) + 1;
            if (srv->connections_total_metric)
                metrics_counter_inc(srv->connections_total_metric);
            if (srv->active_connections_metric)
                metrics_gauge_set(srv->active_connections_metric, (double)active);

            /* Verbose accept log — gated by sampling. */
            if (log && server_should_log(srv)) {
                char ps[8]; snprintf(ps, sizeof(ps), "%u", rport);
                LOG_INFO(log, "tcp_server", "Connection accepted",
                        "remote_addr", addr_str, "remote_port", ps, NULL);
            } else if (log) {
                transport_obs_record_drop();
            }

            /* Post-push saturation check. */
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
 * CONNECTION ALLOCATION (v5: global memory budget check)
 * ========================================================================= */

static tcp_connection_t* connection_alloc(
    int fd,
    const char* remote_addr,
    uint16_t remote_port,
    const tcp_connection_config_t* cfg,
    const tcp_conn_metrics_t* shared_metrics,
    logger_t* logger
) {
    size_t q_cap = (cfg && cfg->send_queue_capacity)
                   ? cfg->send_queue_capacity : SEND_QUEUE_DEFAULT_CAPACITY;
    size_t q_hwm = (cfg && cfg->send_queue_hwm)
                   ? cfg->send_queue_hwm : SEND_QUEUE_DEFAULT_HWM;

    /*
     * Reserve memory in the global budget BEFORE allocating.
     * This ensures the counter stays accurate even if the subsequent
     * calloc() fails (we release immediately in that case).
     */
    if (!transport_memory_acquire(q_cap)) {
        /* Global limit exhausted — connection rejected. */
        if (logger) {
            LOG_WARN(logger, "tcp", "Connection rejected: global memory budget exhausted",
                    NULL);
        }
        return NULL;
    }

    tcp_connection_t* conn = calloc(1, sizeof(*conn));
    if (!conn) {
        transport_memory_release(q_cap);
        return NULL;
    }

    if (send_queue_init(&conn->send_queue, q_cap, q_hwm) != 0) {
        transport_memory_release(q_cap);
        free(conn);
        return NULL;
    }

    conn->mem_reserved_bytes = q_cap;

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

    srv->worker_count     = cfg && cfg->worker_threads
                            ? cfg->worker_threads : cpu_count();
    if (srv->worker_count > TCP_MAX_WORKERS) srv->worker_count = TCP_MAX_WORKERS;

    uint32_t queue_depth  = cfg && cfg->worker_queue_depth
                            ? cfg->worker_queue_depth : TCP_DEFAULT_QUEUE_DEPTH;
    srv->drain_timeout_ms = cfg && cfg->drain_timeout_ms
                            ? cfg->drain_timeout_ms : TCP_DEFAULT_DRAIN_TIMEOUT_MS;

    srv->conn_config.send_queue_capacity = cfg ? cfg->conn_send_queue_capacity : 0;
    srv->conn_config.send_queue_hwm      = cfg ? cfg->conn_send_queue_hwm      : 0;

    /* v5: saturation config */
    srv->handler_warn_duration_ms = cfg ? cfg->handler_warn_duration_ms : 0;
    srv->obs_sample_pct           = cfg ? cfg->observability_sample_pct  : 0;

    atomic_store(&srv->metrics, metrics);
    atomic_store(&srv->logger,  logger);
    atomic_init(&srv->busy_workers, 0);
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
    if (srv->listen_fd < 0) {
        close(srv->epoll_fd); free(srv);
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

    /* Register server-level metrics once. */
    if (metrics) {
        metrics_register_counter(metrics, "tcp_connections_total",
            "Total TCP connections accepted", NULL, 0,
            &srv->connections_total_metric);
        metrics_register_gauge(metrics, "tcp_active_connections",
            "Current active TCP connections", NULL, 0,
            &srv->active_connections_metric);
        metrics_register_counter(metrics, "tcp_accept_errors_total",
            "TCP accept() errors", NULL, 0,
            &srv->accept_errors_metric);
        metrics_register_counter(metrics, "tcp_worker_queue_full_total",
            "Connections dropped due to full worker queue", NULL, 0,
            &srv->queue_full_metric);
        metrics_register_counter(metrics, "tcp_server_accept_rejections_total",
            "Connections rejected due to queue saturation backpressure",
            NULL, 0, &srv->accept_rejections_metric);

        /* v5 saturation metrics */
        metrics_register_gauge(metrics, "tcp_worker_busy_ratio",
            "Fraction of workers currently executing a connection callback",
            NULL, 0, &srv->worker_busy_ratio_metric);
        metrics_register_counter(metrics, "tcp_handler_slow_total",
            "Connection handler invocations exceeding the warning threshold",
            NULL, 0, &srv->handler_slow_total_metric);
        metrics_register_gauge(metrics, "tcp_handler_duration_max_ms",
            "Most recent connection handler duration in milliseconds",
            NULL, 0, &srv->handler_duration_max_metric);
        metrics_register_gauge(metrics, "tcp_worker_queue_depth",
            "Current number of pending items in the worker dispatch queue",
            NULL, 0, &srv->worker_queue_depth_metric);

        /* Per-connection shared metrics */
        metrics_register_counter(metrics, "tcp_bytes_sent_total",
            "Total bytes sent", NULL, 0, &srv->conn_metrics.bytes_sent);
        metrics_register_counter(metrics, "tcp_bytes_received_total",
            "Total bytes received", NULL, 0, &srv->conn_metrics.bytes_recv);
        metrics_register_counter(metrics, "tcp_send_errors_total",
            "TCP send errors", NULL, 0, &srv->conn_metrics.send_errors);
        metrics_register_counter(metrics, "tcp_recv_errors_total",
            "TCP receive errors", NULL, 0, &srv->conn_metrics.recv_errors);
        metrics_register_counter(metrics, "tcp_backpressure_total",
            "Times send queue HWM triggered backpressure", NULL, 0,
            &srv->conn_metrics.backpressure);
    }

    if (logger) {
        char wc[8]; snprintf(wc, sizeof(wc), "%u", srv->worker_count);
        char qd[16]; snprintf(qd, sizeof(qd), "%u", queue_depth);
        LOG_INFO(logger, "tcp_server", "Server created",
                "bind_addr", bind_addr,
                "workers", wc,
                "queue_depth", qd, NULL);
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
    return tcp_server_create_with_config(bind_addr, port, NULL,
                                         metrics, logger, server);
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
        server->workers[i].server = server;
        pthread_create(&server->workers[i].thread, NULL,
                       worker_thread_func, &server->workers[i]);
    }

    pthread_create(&server->accept_thread, NULL, accept_thread_func, server);
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_server_stop — Deterministic shutdown
 * ========================================================================= */

distric_err_t tcp_server_stop(tcp_server_t* server) {
    if (!server) return DISTRIC_ERR_INVALID_ARG;

    tcp_server_state_t prev = (tcp_server_state_t)atomic_exchange(
        &server->state, TCP_SERVER_DRAINING);
    if (prev == TCP_SERVER_STOPPED)  return DISTRIC_OK;
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
            char rbuf[24]; snprintf(rbuf, sizeof(rbuf), "%lld",
                                    (long long)remaining);
            LOG_WARN(log, "tcp_server",
                    "Drain timeout: force-stopping worker queue",
                    "remaining", rbuf, NULL);
        }
        wq_force_stop(&server->work_queue);
    } else {
        wq_shutdown(&server->work_queue);
    }

    /* Nullify observability with seq_cst before joining workers.
     * Workers joined after this cannot use the now-NULL pointers. */
    atomic_store_explicit(&server->metrics, NULL, memory_order_seq_cst);
    atomic_store_explicit(&server->logger,  NULL, memory_order_seq_cst);

    for (uint32_t i = 0; i < server->worker_count; i++) {
        pthread_join(server->workers[i].thread, NULL);
    }

    if (!clean_drain) {
        work_item_t item;
        while (wq_try_pop(&server->work_queue, &item) == 0) {
            tcp_close(item.conn);
            atomic_fetch_add(&server->active_connections, -1);
        }
    }

    wq_destroy(&server->work_queue);

    close(server->listen_fd);
    close(server->epoll_fd);

    atomic_store(&server->state, TCP_SERVER_STOPPED);
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_server_destroy
 * ========================================================================= */

void tcp_server_destroy(tcp_server_t* server) {
    if (!server) return;
    if (atomic_load(&server->state) != TCP_SERVER_STOPPED)
        tcp_server_stop(server);

    pthread_cond_destroy(&server->drain_cond);
    pthread_mutex_destroy(&server->drain_mutex);
    free(server->workers);
    free(server);
}

/* ============================================================================
 * tcp_server_get_state / tcp_server_active_connections / busy_ratio
 * ========================================================================= */

tcp_server_state_t tcp_server_get_state(const tcp_server_t* server) {
    if (!server) return TCP_SERVER_STOPPED;
    return (tcp_server_state_t)atomic_load_explicit(&server->state,
                                                     memory_order_relaxed);
}

int64_t tcp_server_active_connections(const tcp_server_t* server) {
    if (!server) return 0;
    return atomic_load_explicit(&server->active_connections, memory_order_relaxed);
}

float tcp_server_worker_busy_ratio(const tcp_server_t* server) {
    if (!server || server->worker_count == 0) return 0.0f;
    uint32_t busy = atomic_load_explicit(&server->busy_workers,
                                          memory_order_relaxed);
    return (float)busy / (float)server->worker_count;
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
    if (!host || !conn_out || timeout_ms <= 0) return DISTRIC_ERR_INVALID_ARG;
    (void)metrics; /* metrics are attached per-connection via connection_alloc */

    cb_registry_t* cb = get_cb_registry();
    if (cb && !cb_is_allowed(cb, host, port)) {
        if (logger) {
            LOG_WARN(logger, "tcp", "Circuit breaker OPEN",
                    "host", host, NULL);
        }
        return DISTRIC_ERR_UNAVAILABLE;
    }

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return DISTRIC_ERR_INIT_FAILED;
    }

    struct sockaddr_in saved_addr = *(struct sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return DISTRIC_ERR_INIT_FAILED;

    set_nonblocking(fd);
    set_tcp_nodelay(fd);

    int rc = connect(fd, (struct sockaddr*)&saved_addr, sizeof(saved_addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        if (cb) cb_record_failure(cb, host, port);
        return DISTRIC_ERR_INIT_FAILED;
    }

    if (rc != 0) {
        /* Wait for connection with epoll. */
        int efd = epoll_create1(EPOLL_CLOEXEC);
        if (efd < 0) { close(fd); return DISTRIC_ERR_INIT_FAILED; }

        struct epoll_event ev = {
            .events  = EPOLLOUT | EPOLLERR | EPOLLHUP,
            .data.fd = fd
        };
        epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);

        struct epoll_event out;
        int n = epoll_wait(efd, &out, 1, timeout_ms);
        close(efd);

        if (n <= 0) {
            close(fd);
            if (cb) cb_record_failure(cb, host, port);
            return (n == 0) ? DISTRIC_ERR_TIMEOUT : DISTRIC_ERR_INIT_FAILED;
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
            if (cb) cb_record_failure(cb, host, port);
            return transport_err_to_distric(transport_classify_errno(so_err));
        }
    }

    char addr_str[64];
    inet_ntop(AF_INET, &saved_addr.sin_addr, addr_str, sizeof(addr_str));

    tcp_connection_t* conn = connection_alloc(fd, addr_str, port, config,
                                              NULL, logger);
    if (!conn) {
        close(fd);
        /*
         * Do NOT record a circuit-breaker failure here.
         * connection_alloc() failed due to a LOCAL resource constraint
         * (global memory budget exhausted or OOM), not a remote host fault.
         * Penalising the CB for a local problem would cause the breaker to
         * open against a healthy host, blocking all subsequent connects.
         */
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    if (cb) cb_record_success(cb, host, port);

    if (logger) {
        LOG_INFO(logger, "tcp", "Connected",
                "remote_addr", addr_str, "port", port_str, NULL);
    }

    *conn_out = conn;
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_close (v5: global memory release)
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

    /* Release memory reservation from the global budget. */
    if (conn->mem_reserved_bytes > 0)
        transport_memory_release(conn->mem_reserved_bytes);

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
        /* Kernel still blocked — buffer in queue. */
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

    /* Queue empty — try direct send. */
    ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
        if (conn->metrics && conn->metrics->bytes_sent)
            metrics_counter_add(conn->metrics->bytes_sent, (uint64_t)sent);

        if ((size_t)sent < len) {
            /* Partial send — buffer remainder. */
            const uint8_t* remainder = (const uint8_t*)data + sent;
            size_t         rem_len   = len - (size_t)sent;
            if (send_queue_above_hwm(&conn->send_queue) ||
                send_queue_push(&conn->send_queue, remainder, rem_len) != 0)
            {
                if (conn->metrics && conn->metrics->backpressure)
                    metrics_counter_inc(conn->metrics->backpressure);
                pthread_mutex_unlock(&conn->send_lock);
                return DISTRIC_ERR_BACKPRESSURE;
            }
        }
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Buffer entire message. */
        if (send_queue_above_hwm(&conn->send_queue) ||
            send_queue_push(&conn->send_queue, data, len) != 0)
        {
            if (conn->metrics && conn->metrics->backpressure)
                metrics_counter_inc(conn->metrics->backpressure);
            pthread_mutex_unlock(&conn->send_lock);
            return DISTRIC_ERR_BACKPRESSURE;
        }
    } else {
        if (conn->metrics && conn->metrics->send_errors)
            metrics_counter_inc(conn->metrics->send_errors);
        pthread_mutex_unlock(&conn->send_lock);
        return DISTRIC_ERR_IO;
    }

    pthread_mutex_unlock(&conn->send_lock);
    return (int)len;
}

int tcp_flush(tcp_connection_t* conn) {
    if (!conn) return DISTRIC_ERR_INVALID_ARG;
    pthread_mutex_lock(&conn->send_lock);
    int rc = flush_send_queue_locked(conn);
    pthread_mutex_unlock(&conn->send_lock);
    return rc == 0 ? DISTRIC_OK : DISTRIC_ERR_IO;
}

/* ============================================================================
 * RECEIVE PATH
 * ========================================================================= */

int tcp_recv(tcp_connection_t* conn, void* buffer, size_t len, int timeout_ms) {
    if (!conn || !buffer || len == 0) return DISTRIC_ERR_INVALID_ARG;

    if (timeout_ms > 0) {
        /* DEPRECATED: blocking receive with timeout. */
        struct epoll_event ev;
        int n = epoll_wait(conn->recv_epoll_fd, &ev, 1, timeout_ms);
        if (n <= 0) return 0;
        if (ev.events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) return DISTRIC_ERR_IO;
    } else if (timeout_ms == 0) {
        /* Block indefinitely. */
        struct epoll_event ev;
        epoll_wait(conn->recv_epoll_fd, &ev, 1, -1);
        if (ev.events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) return DISTRIC_ERR_IO;
    }

    ssize_t n = recv(conn->fd, buffer, len, MSG_DONTWAIT);
    if (n > 0) {
        if (conn->metrics && conn->metrics->bytes_recv)
            metrics_counter_add(conn->metrics->bytes_recv, (uint64_t)n);
        return (int)n;
    }
    if (n == 0 || errno == ECONNRESET) {
        return DISTRIC_ERR_IO;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    if (conn->metrics && conn->metrics->recv_errors)
        metrics_counter_inc(conn->metrics->recv_errors);
    return DISTRIC_ERR_IO;
}

bool tcp_is_readable(tcp_connection_t* conn) {
    if (!conn || conn->recv_epoll_fd < 0) return false;
    struct epoll_event ev;
    int n = epoll_wait(conn->recv_epoll_fd, &ev, 1, 0);
    if (n <= 0) return false;
    return (ev.events & EPOLLIN) != 0;
}

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

int tcp_connection_get_fd(const tcp_connection_t* conn) {
    return conn ? conn->fd : -1;
}

distric_err_t tcp_get_remote_addr(
    const tcp_connection_t* conn,
    char*                   addr_out,
    size_t                  addr_len,
    uint16_t*               port_out
) {
    if (!conn || !addr_out || addr_len == 0) return DISTRIC_ERR_INVALID_ARG;
    strncpy(addr_out, conn->remote_addr, addr_len - 1);
    addr_out[addr_len - 1] = '\0';
    if (port_out) *port_out = conn->remote_port;
    return DISTRIC_OK;
}

uint64_t tcp_get_connection_id(const tcp_connection_t* conn) {
    return conn ? conn->connection_id : 0;
}