/**
 * @file udp.c
 * @brief DistriC UDP Transport — v4
 *
 * Changes from v3:
 *
 * 1. UDP PEER EVICTION METRIC (Item 8 from gap analysis)
 *    Added udp_peer_evictions_total metric. Incremented in evict_lru_peer()
 *    on every forced eviction (LRU or TTL-expired). Callers can correlate
 *    this metric with drops to detect active IP-spoof flood attacks.
 *
 * 2. STRICT MAX_PEERS ENFORCEMENT
 *    bucket_find_or_create() no longer returns NULL when active_peer_count
 *    equals max_peers without first attempting eviction. The eviction path
 *    is always invoked when at capacity. This closes a defensive gap where
 *    a NULL return bypassed the max cap check for new IPs.
 *
 * 3. EVICTION CORRECTNESS UNDER FLOOD CONDITIONS
 *    evict_lru_peer() now scans the full table (not stopping at gaps) to
 *    guarantee it always finds the true LRU candidate even when the table
 *    has been fragmented by hash collisions. Previously, stopping at gaps
 *    could miss older entries in later positions.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/udp.h"
#include "distric_transport/transport_error.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* ============================================================================
 * TOKEN BUCKET RATE LIMITER WITH LRU EVICTION
 * ========================================================================= */

#define BUCKET_TABLE_SIZE   256u
#define BUCKET_TABLE_MASK   (BUCKET_TABLE_SIZE - 1u)
#define NS_PER_SEC          1000000000LL
#define PEER_DEFAULT_TTL_NS (30LL * NS_PER_SEC)

typedef struct {
    uint32_t         ip_addr;
    uint32_t         max_tokens;
    uint32_t         tokens_per_ns_num;
    _Atomic int64_t  tokens_x1e9;
    int64_t          last_refill_ns;
    int64_t          last_seen_ns;
    bool             active;
} token_bucket_entry_t;

static int64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

/* ============================================================================
 * UDP SOCKET STRUCTURE
 * ========================================================================= */

struct udp_socket_s {
    int      fd;
    uint16_t port;
    char     bind_addr[64];

    bool              rate_limiting_enabled;
    uint32_t          rate_limit_pps;
    uint32_t          burst_size;
    uint32_t          max_peers;
    int64_t           peer_ttl_ns;
    uint32_t          active_peer_count;
    token_bucket_entry_t buckets[BUCKET_TABLE_SIZE];
    pthread_mutex_t   bucket_lock;

    _Atomic uint64_t  drops_rate_limited;
    _Atomic uint64_t  drops_total;
    _Atomic uint64_t  peer_evictions;  /* NEW: total LRU/TTL evictions */

    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           packets_sent_metric;
    metric_t*           packets_recv_metric;
    metric_t*           bytes_sent_metric;
    metric_t*           bytes_recv_metric;
    metric_t*           send_errors_metric;
    metric_t*           recv_errors_metric;
    metric_t*           drops_metric;
    metric_t*           evictions_metric;  /* NEW: udp_peer_evictions_total */
};

/* ============================================================================
 * SOCKET HELPERS
 * ========================================================================= */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ============================================================================
 * TOKEN BUCKET OPERATIONS
 * ========================================================================= */

/*
 * Evict the LRU (or TTL-expired) peer.
 *
 * Scans the ENTIRE table (not stopping at gaps) to find the true oldest
 * entry. This is critical for correctness under IP-flood fragmentation:
 * hash collisions can place entries in non-contiguous slots; stopping at
 * a gap would miss older entries further in the table.
 *
 * Returns the index of the evicted slot.
 * Caller must hold bucket_lock and active_peer_count must be > 0.
 */
static int evict_lru_peer(struct udp_socket_s* sock, int64_t now_ns) {
    int    best_idx = -1;
    int64_t best_ts = INT64_MAX;

    for (uint32_t i = 0; i < BUCKET_TABLE_SIZE; i++) {
        if (!sock->buckets[i].active) continue;

        /* Preferentially evict TTL-expired entries immediately */
        int64_t age = now_ns - sock->buckets[i].last_seen_ns;
        if (age > sock->peer_ttl_ns) {
            sock->buckets[i].active = false;
            sock->active_peer_count--;
            atomic_fetch_add(&sock->peer_evictions, 1);
            if (sock->evictions_metric)
                metrics_counter_inc(sock->evictions_metric);
            return (int)i;
        }

        if (sock->buckets[i].last_seen_ns < best_ts) {
            best_ts  = sock->buckets[i].last_seen_ns;
            best_idx = (int)i;
        }
    }

    /* Evict true LRU */
    if (best_idx >= 0) {
        sock->buckets[best_idx].active = false;
        sock->active_peer_count--;
        atomic_fetch_add(&sock->peer_evictions, 1);
        if (sock->evictions_metric)
            metrics_counter_inc(sock->evictions_metric);
    }
    return best_idx;
}

/*
 * Find or create a token bucket for @p ip.
 *
 * When at capacity (active_peer_count >= max_peers), eviction is ALWAYS
 * attempted. Returns NULL only if eviction itself found no candidates —
 * which cannot happen when max_peers > 0. Defensive NULL check remains.
 *
 * Must be called with bucket_lock held.
 */
static token_bucket_entry_t* bucket_find_or_create(
    struct udp_socket_s* sock,
    uint32_t ip,
    int64_t now_ns
) {
    uint32_t hash = ip & BUCKET_TABLE_MASK;

    /* Linear probe for existing entry */
    for (uint32_t probe = 0; probe < BUCKET_TABLE_SIZE; probe++) {
        uint32_t idx = (hash + probe) & BUCKET_TABLE_MASK;
        token_bucket_entry_t* e = &sock->buckets[idx];
        if (e->active && e->ip_addr == ip) {
            e->last_seen_ns = now_ns;
            return e;
        }
        if (!e->active) {
            /* Empty slot */
            if (sock->active_peer_count >= sock->max_peers) {
                /* At capacity: must evict before using this slot */
                int evict_idx = evict_lru_peer(sock, now_ns);
                if (evict_idx < 0) return NULL; /* Defensive; should not occur */
                e = &sock->buckets[evict_idx];
            }
            e->ip_addr             = ip;
            e->max_tokens          = sock->burst_size;
            e->tokens_per_ns_num   = sock->rate_limit_pps;
            e->last_refill_ns      = now_ns;
            e->last_seen_ns        = now_ns;
            e->active              = true;
            atomic_init(&e->tokens_x1e9, (int64_t)sock->burst_size * NS_PER_SEC);
            sock->active_peer_count++;
            return e;
        }
    }

    /* Table completely full (all slots active due to probing collision chain) */
    if (sock->active_peer_count >= sock->max_peers) {
        int evict_idx = evict_lru_peer(sock, now_ns);
        if (evict_idx < 0) return NULL;

        token_bucket_entry_t* e = &sock->buckets[evict_idx];
        e->ip_addr             = ip;
        e->max_tokens          = sock->burst_size;
        e->tokens_per_ns_num   = sock->rate_limit_pps;
        e->last_refill_ns      = now_ns;
        e->last_seen_ns        = now_ns;
        e->active              = true;
        atomic_init(&e->tokens_x1e9, (int64_t)sock->burst_size * NS_PER_SEC);
        sock->active_peer_count++;
        return e;
    }

    return NULL;
}

/*
 * Try to consume one token from peer bucket.
 *
 * Hot path: existing entries use atomic ops only (no lock).
 * Slow path: new peer acquires bucket_lock briefly.
 */
static bool bucket_try_consume(struct udp_socket_s* sock, uint32_t peer_ip) {
    if (!sock->rate_limiting_enabled) return true;

    int64_t  now  = monotonic_ns();
    uint32_t hash = peer_ip & BUCKET_TABLE_MASK;

    /* Fast path: scan for existing active entry */
    for (uint32_t probe = 0; probe < BUCKET_TABLE_SIZE; probe++) {
        uint32_t idx = (hash + probe) & BUCKET_TABLE_MASK;
        token_bucket_entry_t* e = &sock->buckets[idx];
        if (!e->active) break;
        if (e->ip_addr != peer_ip) continue;

        /* Refill tokens */
        int64_t elapsed = now - e->last_refill_ns;
        if (elapsed > 0) {
            int64_t new_tokens = (int64_t)e->tokens_per_ns_num * elapsed;
            int64_t max_t      = (int64_t)e->max_tokens * NS_PER_SEC;
            int64_t cur        = atomic_load_explicit(&e->tokens_x1e9, memory_order_relaxed);
            int64_t refilled   = cur + new_tokens;
            if (refilled > max_t) refilled = max_t;
            atomic_store_explicit(&e->tokens_x1e9, refilled, memory_order_relaxed);
            e->last_refill_ns = now;
        }
        e->last_seen_ns = now;

        int64_t prev = atomic_fetch_sub(&e->tokens_x1e9, NS_PER_SEC);
        if (prev >= NS_PER_SEC) return true;
        atomic_fetch_add(&e->tokens_x1e9, NS_PER_SEC);
        return false;
    }

    /* Slow path: new peer */
    pthread_mutex_lock(&sock->bucket_lock);
    token_bucket_entry_t* e = bucket_find_or_create(sock, peer_ip, now);
    if (!e) {
        pthread_mutex_unlock(&sock->bucket_lock);
        return false;
    }
    int64_t prev = atomic_fetch_sub(&e->tokens_x1e9, NS_PER_SEC);
    bool allowed = (prev >= NS_PER_SEC);
    if (!allowed) atomic_fetch_add(&e->tokens_x1e9, NS_PER_SEC);
    pthread_mutex_unlock(&sock->bucket_lock);
    return allowed;
}

/* ============================================================================
 * udp_socket_create
 * ========================================================================= */

distric_err_t udp_socket_create(
    const char*                    bind_addr,
    uint16_t                       port,
    const udp_rate_limit_config_t* rate_cfg,
    metrics_registry_t*            metrics,
    logger_t*                      logger,
    udp_socket_t**                 sock_out
) {
    if (!bind_addr || !sock_out) return DISTRIC_ERR_INVALID_ARG;

    udp_socket_t* sock = calloc(1, sizeof(*sock));
    if (!sock) return DISTRIC_ERR_ALLOC_FAILURE;

    sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock->fd < 0) { free(sock); return DISTRIC_ERR_INIT_FAILED; }

    set_nonblocking(sock->fd);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        transport_err_t terr = transport_classify_errno(errno);
        if (logger) {
            char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
            LOG_ERROR(logger, "udp", "Bind failed",
                     "bind_addr", bind_addr, "port", ps,
                     "error", transport_err_str(terr), NULL);
        }
        close(sock->fd);
        free(sock);
        return transport_err_to_distric(terr);
    }

    sock->port    = port;
    sock->metrics = metrics;
    sock->logger  = logger;
    strncpy(sock->bind_addr, bind_addr, sizeof(sock->bind_addr) - 1);

    if (rate_cfg && rate_cfg->rate_limit_pps > 0) {
        sock->rate_limiting_enabled = true;
        sock->rate_limit_pps        = rate_cfg->rate_limit_pps;
        sock->burst_size            = rate_cfg->burst_size
                                      ? rate_cfg->burst_size
                                      : rate_cfg->rate_limit_pps * 2;
        sock->max_peers             = rate_cfg->max_peers
                                      ? rate_cfg->max_peers
                                      : BUCKET_TABLE_SIZE;
        if (sock->max_peers > BUCKET_TABLE_SIZE)
            sock->max_peers = BUCKET_TABLE_SIZE;
        sock->peer_ttl_ns = rate_cfg->peer_ttl_s
                            ? (int64_t)rate_cfg->peer_ttl_s * NS_PER_SEC
                            : PEER_DEFAULT_TTL_NS;
    }

    atomic_init(&sock->drops_rate_limited, 0);
    atomic_init(&sock->drops_total,        0);
    atomic_init(&sock->peer_evictions,     0);
    pthread_mutex_init(&sock->bucket_lock, NULL);

    if (metrics) {
        metrics_register_counter(metrics, "udp_packets_sent_total",
            "UDP packets sent", NULL, 0, &sock->packets_sent_metric);
        metrics_register_counter(metrics, "udp_packets_recv_total",
            "UDP packets received", NULL, 0, &sock->packets_recv_metric);
        metrics_register_counter(metrics, "udp_bytes_sent_total",
            "UDP bytes sent", NULL, 0, &sock->bytes_sent_metric);
        metrics_register_counter(metrics, "udp_bytes_recv_total",
            "UDP bytes received", NULL, 0, &sock->bytes_recv_metric);
        metrics_register_counter(metrics, "udp_send_errors_total",
            "UDP send errors", NULL, 0, &sock->send_errors_metric);
        metrics_register_counter(metrics, "udp_recv_errors_total",
            "UDP receive errors", NULL, 0, &sock->recv_errors_metric);
        metrics_register_counter(metrics, "udp_packets_dropped_total",
            "UDP packets dropped (rate limited)", NULL, 0, &sock->drops_metric);
        metrics_register_counter(metrics, "udp_peer_evictions_total",
            "UDP peer table LRU/TTL evictions (DoS indicator)",
            NULL, 0, &sock->evictions_metric);
    }

    if (logger) {
        char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
        LOG_INFO(logger, "udp", "UDP socket created",
                "bind_addr", bind_addr, "port", ps, NULL);
    }

    *sock_out = sock;
    return DISTRIC_OK;
}

/* ============================================================================
 * udp_send
 * ========================================================================= */

int udp_send(
    udp_socket_t* sock,
    const void*   data,
    size_t        len,
    const char*   dest_addr,
    uint16_t      dest_port
) {
    if (!sock || !data || len == 0 || !dest_addr) return DISTRIC_ERR_INVALID_ARG;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(dest_port);
    if (inet_pton(AF_INET, dest_addr, &dst.sin_addr) <= 0)
        return DISTRIC_ERR_INVALID_ARG;

    ssize_t sent = sendto(sock->fd, data, len, MSG_DONTWAIT,
                          (struct sockaddr*)&dst, sizeof(dst));
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return DISTRIC_ERR_BACKPRESSURE;
        transport_err_t terr = transport_classify_errno(errno);
        if (sock->send_errors_metric) metrics_counter_inc(sock->send_errors_metric);
        if (sock->logger) {
            LOG_ERROR(sock->logger, "udp", "Send failed",
                     "error", transport_err_str(terr), NULL);
        }
        return DISTRIC_ERR_IO;
    }

    if (sock->packets_sent_metric) metrics_counter_inc(sock->packets_sent_metric);
    if (sock->bytes_sent_metric)   metrics_counter_add(sock->bytes_sent_metric, (uint64_t)sent);
    return (int)sent;
}

/* ============================================================================
 * udp_recv
 * ========================================================================= */

int udp_recv(
    udp_socket_t* sock,
    void*         buffer,
    size_t        len,
    char*         src_addr,
    uint16_t*     src_port,
    int           timeout_ms
) {
    if (!sock || !buffer || len == 0) return DISTRIC_ERR_INVALID_ARG;

    if (timeout_ms >= 0) {
        int efd = epoll_create1(0);
        if (efd < 0) return DISTRIC_ERR_IO;

        struct epoll_event ev = { .events = EPOLLIN, .data.fd = sock->fd };
        epoll_ctl(efd, EPOLL_CTL_ADD, sock->fd, &ev);

        struct epoll_event evout[1];
        int nev = epoll_wait(efd, evout, 1, timeout_ms == 0 ? -1 : timeout_ms);
        close(efd);

        if (nev == 0) return 0;
        if (nev < 0)  return 0;
    }

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    ssize_t received = recvfrom(sock->fd, buffer, len, 0,
                                (struct sockaddr*)&peer_addr, &peer_len);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        transport_err_t terr = transport_classify_errno(errno);
        if (sock->recv_errors_metric) metrics_counter_inc(sock->recv_errors_metric);
        if (sock->logger) {
            LOG_ERROR(sock->logger, "udp", "Recv failed",
                     "error", transport_err_str(terr), NULL);
        }
        return DISTRIC_ERR_IO;
    }

    uint32_t peer_ip = ntohl(peer_addr.sin_addr.s_addr);
    if (!bucket_try_consume(sock, peer_ip)) {
        atomic_fetch_add(&sock->drops_rate_limited, 1);
        atomic_fetch_add(&sock->drops_total, 1);
        if (sock->drops_metric) metrics_counter_inc(sock->drops_metric);
        return 0;
    }

    if (sock->packets_recv_metric) metrics_counter_inc(sock->packets_recv_metric);
    if (sock->bytes_recv_metric)   metrics_counter_add(sock->bytes_recv_metric, (uint64_t)received);

    if (src_addr) inet_ntop(AF_INET, &peer_addr.sin_addr, src_addr, 64);
    if (src_port) *src_port = ntohs(peer_addr.sin_port);

    return (int)received;
}

/* ============================================================================
 * udp_get_drop_count / udp_get_eviction_count / udp_close
 * ========================================================================= */

uint64_t udp_get_drop_count(udp_socket_t* sock) {
    if (!sock) return 0;
    return atomic_load(&sock->drops_total);
}

uint64_t udp_get_eviction_count(udp_socket_t* sock) {
    if (!sock) return 0;
    return atomic_load(&sock->peer_evictions);
}

void udp_close(udp_socket_t* sock) {
    if (!sock) return;

    if (sock->logger) {
        char ps[8]; snprintf(ps, sizeof(ps), "%u", sock->port);
        char ds[24];
        snprintf(ds, sizeof(ds), "%llu",
                 (unsigned long long)atomic_load(&sock->drops_total));
        char es[24];
        snprintf(es, sizeof(es), "%llu",
                 (unsigned long long)atomic_load(&sock->peer_evictions));
        LOG_INFO(sock->logger, "udp", "UDP socket closed",
                "port", ps, "total_drops", ds, "total_evictions", es, NULL);
    }

    close(sock->fd);
    pthread_mutex_destroy(&sock->bucket_lock);
    free(sock);
}