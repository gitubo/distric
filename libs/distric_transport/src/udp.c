/**
 * @file udp.c
 * @brief DistriC UDP Transport — Refactored v2
 *
 * Key changes from v1:
 *
 * 1. RATE LIMITING (per-peer token bucket)
 *    Each unique source IP has a token_bucket_entry_t. On every receive the
 *    bucket is checked: if tokens == 0 the packet is dropped and a counter
 *    is incremented atomically (no lock on hot path for existing entries).
 *    New entries are created under a short mutex, then released.
 *    The bucket table uses linear probing (fixed-size, no rehashing).
 *
 * 2. OBSERVABILITY DECOUPLED FROM HOT PATH
 *    - Per-packet DEBUG logging removed; errors and lifecycle events only.
 *    - Metrics use lock-free counter increments (distric_obs guarantees).
 *    - Drop count is a separate _Atomic uint64_t for cheap reads.
 *
 * 3. STABLE ERROR TAXONOMY
 *    errno values classified via transport_classify_errno().
 *
 * 4. THREAD-SAFE RESOLUTION
 *    gethostbyname() replaced with getaddrinfo() (thread-safe, reentrant).
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
 * TOKEN BUCKET RATE LIMITER
 * ========================================================================= */

/*
 * We maintain a fixed-size hash table of per-peer token buckets.
 * Bucket count is a power of two so we can use bitmask indexing.
 * Eviction is simple LRU-clock (we don't evict; old entries just get reused
 * if the table is full — acceptable for production cluster sizes).
 *
 * Token replenishment uses wall-clock time in nanoseconds.
 */

#define BUCKET_TABLE_SIZE   256u          /* Must be power of 2 */
#define BUCKET_TABLE_MASK   (BUCKET_TABLE_SIZE - 1u)
#define NS_PER_SEC          1000000000LL

typedef struct {
    uint32_t ip_addr;             /* Peer IPv4 address in host byte order */
    uint32_t max_tokens;          /* Burst size                           */
    uint32_t tokens_per_ns_num;   /* tokens = tokens_per_ns_num / 10^9   */
    _Atomic int64_t  tokens_x1e9; /* tokens × 10^9 (avoids floats)       */
    int64_t  last_refill_ns;      /* Monotonic time of last refill        */
    bool     active;
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
    token_bucket_entry_t buckets[BUCKET_TABLE_SIZE];
    pthread_mutex_t   bucket_lock;  /* Only held when inserting new entries */

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
 * Find or create a bucket for @p ip (host byte order).
 * Returns pointer to bucket, or NULL if table is full (rate limiting skipped).
 * Called with bucket_lock held for new entries; not held for lookups.
 */
static token_bucket_entry_t* bucket_find_or_create(
    udp_socket_t* sock, uint32_t ip
) {
    /* Linear probe starting at ip hash */
    uint32_t idx = (ip ^ (ip >> 16)) & BUCKET_TABLE_MASK;

    for (uint32_t i = 0; i < BUCKET_TABLE_SIZE; i++) {
        token_bucket_entry_t* b = &sock->buckets[(idx + i) & BUCKET_TABLE_MASK];
        if (b->active && b->ip_addr == ip) return b;
        if (!b->active) {
            /* New entry — caller must hold bucket_lock */
            b->ip_addr         = ip;
            b->max_tokens      = sock->burst_size;
            b->tokens_per_ns_num = sock->rate_limit_pps;
            /* Start full */
            atomic_store(&b->tokens_x1e9, (int64_t)sock->burst_size * NS_PER_SEC);
            b->last_refill_ns  = monotonic_ns();
            b->active          = true;
            return b;
        }
    }
    return NULL;  /* Table full; skip rate limiting for this peer */
}

/**
 * Consume one token from @p bucket. Replenishes based on elapsed time first.
 * Returns true if allowed, false if rate limit exceeded (drop).
 * Lock-free for the token CAS; brief lock only when creating new entries.
 */
static bool bucket_try_consume(
    udp_socket_t* sock, uint32_t ip
) {
    if (!sock->rate_limiting_enabled) return true;

    /* Fast path: find existing bucket */
    uint32_t idx = (ip ^ (ip >> 16)) & BUCKET_TABLE_MASK;
    token_bucket_entry_t* b = NULL;

    for (uint32_t i = 0; i < BUCKET_TABLE_SIZE; i++) {
        token_bucket_entry_t* candidate =
            &sock->buckets[(idx + i) & BUCKET_TABLE_MASK];
        if (candidate->active && candidate->ip_addr == ip) {
            b = candidate;
            break;
        }
        if (!candidate->active) break;
    }

    if (!b) {
        /* New peer: create entry under lock */
        pthread_mutex_lock(&sock->bucket_lock);
        b = bucket_find_or_create(sock, ip);
        pthread_mutex_unlock(&sock->bucket_lock);
        if (!b) return true;  /* Table full; allow */
    }

    /* Replenish tokens based on elapsed time */
    int64_t now      = monotonic_ns();
    int64_t elapsed  = now - b->last_refill_ns;
    if (elapsed > 0) {
        int64_t new_tokens = elapsed * (int64_t)b->tokens_per_ns_num;
        int64_t max_x1e9  = (int64_t)b->max_tokens * NS_PER_SEC;

        /* Saturating add into tokens_x1e9 */
        int64_t old = atomic_load(&b->tokens_x1e9);
        int64_t nv  = old + new_tokens;
        if (nv > max_x1e9) nv = max_x1e9;
        /* Best-effort CAS; if it races, we'll replenish next call */
        atomic_compare_exchange_weak(&b->tokens_x1e9, &old, nv);
        b->last_refill_ns = now;
    }

    /* Consume one packet worth of tokens */
    int64_t one_token = NS_PER_SEC;  /* 1 packet = 1 full token */
    int64_t prev = atomic_fetch_sub(&b->tokens_x1e9, one_token);
    if (prev < one_token) {
        /* Over-subtracted — put it back and deny */
        atomic_fetch_add(&b->tokens_x1e9, one_token);
        return false;
    }
    return true;
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

    strncpy(sock->bind_addr, bind_addr, sizeof(sock->bind_addr) - 1);
    sock->port    = port;
    sock->metrics = metrics;
    sock->logger  = logger;

    atomic_init(&sock->drops_rate_limited, 0);
    atomic_init(&sock->drops_total, 0);
    pthread_mutex_init(&sock->bucket_lock, NULL);

    /* Configure rate limiting */
    if (rate_cfg && rate_cfg->rate_limit_pps > 0) {
        sock->rate_limiting_enabled = true;
        sock->rate_limit_pps        = rate_cfg->rate_limit_pps;
        sock->burst_size = rate_cfg->burst_size > 0
                           ? rate_cfg->burst_size
                           : rate_cfg->rate_limit_pps * 2;
    }

    /* Create and bind socket */
    sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock->fd < 0) {
        free(sock);
        return DISTRIC_ERR_INIT_FAILED;
    }

    set_nonblocking(sock->fd);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        transport_err_t terr = transport_classify_errno(errno);
        if (logger) {
            char port_str[8];
            snprintf(port_str, sizeof(port_str), "%u", port);
            LOG_ERROR(logger, "udp", "Bind failed",
                     "bind_addr", bind_addr, "port", port_str,
                     "error", transport_err_str(terr), NULL);
        }
        close(sock->fd);
        free(sock);
        return transport_err_to_distric(terr);
    }

    /* Register metrics */
    if (metrics) {
        metrics_register_counter(metrics, "udp_packets_sent_total",
            "Total UDP packets sent", NULL, 0, &sock->packets_sent_metric);
        metrics_register_counter(metrics, "udp_packets_received_total",
            "Total UDP packets received", NULL, 0, &sock->packets_recv_metric);
        metrics_register_counter(metrics, "udp_bytes_sent_total",
            "Total UDP bytes sent", NULL, 0, &sock->bytes_sent_metric);
        metrics_register_counter(metrics, "udp_bytes_received_total",
            "Total UDP bytes received", NULL, 0, &sock->bytes_recv_metric);
        metrics_register_counter(metrics, "udp_send_errors_total",
            "Total UDP send errors", NULL, 0, &sock->send_errors_metric);
        metrics_register_counter(metrics, "udp_recv_errors_total",
            "Total UDP receive errors", NULL, 0, &sock->recv_errors_metric);
        metrics_register_counter(metrics, "udp_packets_dropped_total",
            "UDP packets dropped (rate limit + errors)", NULL, 0, &sock->drops_metric);
    }

    if (logger) {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", port);
        char rl_str[32] = "disabled";
        if (sock->rate_limiting_enabled)
            snprintf(rl_str, sizeof(rl_str), "%u pps", sock->rate_limit_pps);
        LOG_INFO(logger, "udp", "UDP socket created",
                "bind_addr", bind_addr,
                "port", port_str,
                "rate_limit", rl_str, NULL);
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
    if (len > 65507) return DISTRIC_ERR_INVALID_ARG;

    /* Resolve destination (getaddrinfo is thread-safe) */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", dest_port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = NULL;
    if (getaddrinfo(dest_addr, port_str, &hints, &res) != 0 || !res) {
        if (sock->send_errors_metric) metrics_counter_inc(sock->send_errors_metric);
        if (res) freeaddrinfo(res);
        return DISTRIC_ERR_INVALID_ARG;
    }

    ssize_t sent = sendto(sock->fd, data, len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (sock->drops_metric) metrics_counter_inc(sock->drops_metric);
            atomic_fetch_add(&sock->drops_total, 1);
            return DISTRIC_ERR_BACKPRESSURE;
        }
        transport_err_t terr = transport_classify_errno(errno);
        if (sock->send_errors_metric) metrics_counter_inc(sock->send_errors_metric);
        if (sock->logger) {
            LOG_ERROR(sock->logger, "udp", "Send failed",
                     "error", transport_err_str(terr), NULL);
        }
        return DISTRIC_ERR_IO;
    }

    /* Hot-path metric updates: lock-free counters only */
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

    /* Wait for datagram using epoll */
    if (timeout_ms >= 0) {
        int efd = epoll_create1(0);
        if (efd < 0) return DISTRIC_ERR_IO;

        struct epoll_event ev = {
            .events  = EPOLLIN,
            .data.fd = sock->fd
        };
        epoll_ctl(efd, EPOLL_CTL_ADD, sock->fd, &ev);

        struct epoll_event events[1];
        int nev = epoll_wait(efd, events, 1, timeout_ms == 0 ? -1 : timeout_ms);
        close(efd);

        if (nev == 0) return 0;    /* Timeout */
        if (nev < 0)  return 0;    /* Interrupted; treat as no data */
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

    /* Apply per-peer rate limiting */
    uint32_t peer_ip = ntohl(peer_addr.sin_addr.s_addr);
    if (!bucket_try_consume(sock, peer_ip)) {
        /* Drop the packet: rate limit exceeded */
        atomic_fetch_add(&sock->drops_rate_limited, 1);
        atomic_fetch_add(&sock->drops_total, 1);
        if (sock->drops_metric) metrics_counter_inc(sock->drops_metric);
        return 0;  /* Caller sees "no data" */
    }

    /* Accepted: update metrics (lock-free) */
    if (sock->packets_recv_metric) metrics_counter_inc(sock->packets_recv_metric);
    if (sock->bytes_recv_metric)   metrics_counter_add(sock->bytes_recv_metric, (uint64_t)received);

    /* Fill caller's optional output fields */
    if (src_addr) {
        inet_ntop(AF_INET, &peer_addr.sin_addr, src_addr, 64);
    }
    if (src_port) {
        *src_port = ntohs(peer_addr.sin_port);
    }

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
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", sock->port);
        char drops_str[24];
        snprintf(drops_str, sizeof(drops_str), "%llu",
                 (unsigned long long)atomic_load(&sock->drops_total));
        LOG_INFO(sock->logger, "udp", "UDP socket closed",
                "port", port_str,
                "total_drops", drops_str, NULL);
    }

    close(sock->fd);
    pthread_mutex_destroy(&sock->bucket_lock);
    free(sock);
}