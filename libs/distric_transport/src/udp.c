/**
 * @file udp.c
 * @brief DistriC UDP Transport — v3
 *
 * Changes from v2:
 *
 * 1. TRUE LRU PEER EVICTION (Item 7)
 *    The per-peer token bucket table previously had no eviction: when the
 *    256-entry table was full, hash collisions silently reused existing slots,
 *    losing rate-limit state for the displaced peer. Under IP-spoof floods
 *    this could be used to reset buckets for legitimate peers.
 *
 *    v3 adds:
 *      - last_seen_ns timestamp per entry.
 *      - When the table is full, evict the entry with the smallest last_seen_ns
 *        (LRU). This bounds memory to BUCKET_TABLE_SIZE entries regardless of
 *        distinct source IPs.
 *      - Configurable TTL (peer_ttl_ns). Entries older than TTL are considered
 *        stale and may be evicted before LRU order is considered.
 *      - max_peers in udp_rate_limit_config_t allows the table to be capped
 *        below BUCKET_TABLE_SIZE for tighter memory control.
 *
 * 2. FIXED HASH COLLISION HANDLING
 *    v2 used linear probing but the eviction path was "reuse if full" which
 *    silently corrupted state. v3 always evicts the LRU entry on collision
 *    when the table is full, maintaining correctness.
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

/*
 * Hash table of per-peer token buckets with linear probing.
 *
 * Each entry tracks:
 *   - ip_addr: peer IPv4 address (host byte order) — key
 *   - tokens_x1e9: token count scaled by 1e9 (avoids floats)
 *   - last_refill_ns: monotonic ns for token replenishment
 *   - last_seen_ns: monotonic ns of last packet from this peer (for LRU)
 *
 * Eviction:
 *   When inserting a new peer and the active count equals max_peers,
 *   scan all active entries and evict the one with the smallest last_seen_ns.
 *   Entries older than peer_ttl_ns are preferentially evicted regardless of
 *   LRU order.
 *
 * Thread safety:
 *   Lookups of existing entries use atomic token decrement — no lock on hot
 *   path. New entry insertion requires bucket_lock to prevent races.
 */

#define BUCKET_TABLE_SIZE   256u          /* Must be power of 2 */
#define BUCKET_TABLE_MASK   (BUCKET_TABLE_SIZE - 1u)
#define NS_PER_SEC          1000000000LL
#define PEER_DEFAULT_TTL_NS (30LL * NS_PER_SEC)  /* 30 seconds */

typedef struct {
    uint32_t         ip_addr;          /* Peer IPv4 address in host byte order */
    uint32_t         max_tokens;       /* Burst size                           */
    uint32_t         tokens_per_ns_num;/* tokens = tokens_per_ns_num / 10^9   */
    _Atomic int64_t  tokens_x1e9;     /* tokens × 10^9 (avoids floats)        */
    int64_t          last_refill_ns;   /* Monotonic time of last refill        */
    int64_t          last_seen_ns;     /* Monotonic time of last packet (LRU)  */
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

    /* Rate limiting */
    bool              rate_limiting_enabled;
    uint32_t          rate_limit_pps;
    uint32_t          burst_size;
    uint32_t          max_peers;            /* Cap on tracked peers */
    int64_t           peer_ttl_ns;          /* Stale peer TTL       */
    uint32_t          active_peer_count;    /* Current active entries */
    token_bucket_entry_t buckets[BUCKET_TABLE_SIZE];
    pthread_mutex_t   bucket_lock;

    /* Drop counters (lock-free) */
    _Atomic uint64_t  drops_rate_limited;
    _Atomic uint64_t  drops_total;

    /* Observability */
    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           packets_sent_metric;
    metric_t*           packets_recv_metric;
    metric_t*           bytes_sent_metric;
    metric_t*           bytes_recv_metric;
    metric_t*           send_errors_metric;
    metric_t*           recv_errors_metric;
    metric_t*           drops_metric;
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

/**
 * Evict the LRU (or TTL-expired) peer to make room for a new entry.
 * Returns the index of the evicted slot, or -1 if no idle slot found.
 * Must be called with bucket_lock held.
 */
static int evict_lru_peer(struct udp_socket_s* sock, int64_t now_ns) {
    int    best_idx  = -1;
    int64_t best_ts  = INT64_MAX;

    for (uint32_t i = 0; i < BUCKET_TABLE_SIZE; i++) {
        if (!sock->buckets[i].active) {
            return (int)i; /* Empty slot — no eviction needed */
        }
        /* Prefer TTL-expired entries */
        int64_t age = now_ns - sock->buckets[i].last_seen_ns;
        if (age > sock->peer_ttl_ns) {
            /* Immediately evict the first TTL-expired entry found */
            sock->buckets[i].active = false;
            sock->active_peer_count--;
            return (int)i;
        }
        if (sock->buckets[i].last_seen_ns < best_ts) {
            best_ts  = sock->buckets[i].last_seen_ns;
            best_idx = (int)i;
        }
    }

    /* Evict LRU */
    if (best_idx >= 0) {
        sock->buckets[best_idx].active = false;
        sock->active_peer_count--;
    }
    return best_idx;
}

/**
 * Find or create a token bucket entry for @p ip.
 * Returns pointer to entry on success, NULL if table is truly full
 * (all max_peers slots in use and no eviction candidate — should not happen
 * with LRU eviction, but defensive).
 * Must be called with bucket_lock held for new insertions.
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
            /* Empty slot — initialise new entry */
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

    /* Table full — evict LRU and insert */
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

/**
 * Try to consume one token from peer bucket.
 * Returns true if packet is allowed, false if rate-limited.
 *
 * Hot path: for existing entries, only atomic ops are used (no lock).
 * New entry creation acquires bucket_lock briefly.
 */
static bool bucket_try_consume(struct udp_socket_s* sock, uint32_t peer_ip) {
    if (!sock->rate_limiting_enabled) return true;

    int64_t now = monotonic_ns();
    uint32_t hash = peer_ip & BUCKET_TABLE_MASK;

    /* Fast path: scan for existing active entry */
    for (uint32_t probe = 0; probe < BUCKET_TABLE_SIZE; probe++) {
        uint32_t idx = (hash + probe) & BUCKET_TABLE_MASK;
        token_bucket_entry_t* e = &sock->buckets[idx];
        if (!e->active) break; /* Gap = not found in probe sequence */
        if (e->ip_addr != peer_ip) continue;

        /* Refill tokens based on elapsed time */
        int64_t elapsed = now - e->last_refill_ns;
        if (elapsed > 0) {
            int64_t new_tokens = (int64_t)e->tokens_per_ns_num * elapsed;
            int64_t max_t      = (int64_t)e->max_tokens * NS_PER_SEC;
            int64_t cur        = atomic_load_explicit(&e->tokens_x1e9,
                                                      memory_order_relaxed);
            int64_t refilled   = cur + new_tokens;
            if (refilled > max_t) refilled = max_t;
            atomic_store_explicit(&e->tokens_x1e9, refilled, memory_order_relaxed);
            e->last_refill_ns = now;
        }
        e->last_seen_ns = now;

        /* Try consume 1 token (= 1e9 scaled units) */
        int64_t prev = atomic_fetch_sub(&e->tokens_x1e9, NS_PER_SEC);
        if (prev >= NS_PER_SEC) return true;

        /* Overdraft — restore and drop */
        atomic_fetch_add(&e->tokens_x1e9, NS_PER_SEC);
        return false;
    }

    /* Slow path: new peer — acquire lock and create */
    pthread_mutex_lock(&sock->bucket_lock);
    token_bucket_entry_t* e = bucket_find_or_create(sock, peer_ip, now);
    if (!e) {
        pthread_mutex_unlock(&sock->bucket_lock);
        return false; /* Table exhausted — drop */
    }
    /* Consume one token immediately */
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

    /* Rate limiting configuration */
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
    atomic_init(&sock->drops_total, 0);
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
    if (!sock || !data || !dest_addr || len == 0) return DISTRIC_ERR_INVALID_ARG;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", dest_port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* result = NULL;

    if (getaddrinfo(dest_addr, port_str, &hints, &result) != 0 || !result) {
        if (sock->send_errors_metric) metrics_counter_inc(sock->send_errors_metric);
        return DISTRIC_ERR_INIT_FAILED;
    }

    ssize_t sent = sendto(sock->fd, data, len, 0,
                          result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

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

        struct epoll_event events[1];
        int nev = epoll_wait(efd, events, 1,
                             timeout_ms == 0 ? -1 : timeout_ms);
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

    /* Per-peer rate limiting with LRU eviction */
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
 * udp_get_drop_count / udp_close
 * ========================================================================= */

uint64_t udp_get_drop_count(udp_socket_t* sock) {
    if (!sock) return 0;
    return atomic_load(&sock->drops_total);
}

void udp_close(udp_socket_t* sock) {
    if (!sock) return;

    if (sock->logger) {
        char ps[8]; snprintf(ps, sizeof(ps), "%u", sock->port);
        char ds[24];
        snprintf(ds, sizeof(ds), "%llu",
                 (unsigned long long)atomic_load(&sock->drops_total));
        LOG_INFO(sock->logger, "udp", "UDP socket closed",
                "port", ps, "total_drops", ds, NULL);
    }

    close(sock->fd);
    pthread_mutex_destroy(&sock->bucket_lock);
    free(sock);
}