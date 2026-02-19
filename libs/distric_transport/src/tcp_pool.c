/**
 * @file tcp_pool.c
 * @brief TCP Connection Pool — v3 (O(1) hash + LRU)
 *
 * Changes from v2:
 *
 * 1. O(1) HASH LOOKUP (Item 6)
 *    Replaces O(n) linked-list scan with a hash table keyed by host:port.
 *    POOL_HASH_BUCKETS = 64 (configurable at compile time). Each bucket
 *    holds a singly-linked chain. Chain length is O(1) amortized for
 *    typical pool sizes (< 1000 connections × 64 buckets → ~15 entries/bucket
 *    worst case, usually 1–3).
 *
 * 2. DOUBLY-LINKED LRU EVICTION LIST (Item 6)
 *    All entries participate in a global LRU doubly-linked list ordered by
 *    last_used_ms. When the pool is full, the LRU idle entry is evicted.
 *    O(1) touch (move to head), O(1) eviction (remove tail).
 *
 * 3. METRICS REGISTERED ONCE (Item 4)
 *    Pool metrics are registered at pool_create time. No per-acquire
 *    registration.
 *
 * 4. SHUTDOWN SAFETY (Item 2)
 *    logger/metrics pointers are NULLed after the pool is fully shut down.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/tcp_pool.h"
#include "distric_transport/transport_error.h"
#include <distric_obs.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

/* ============================================================================
 * CONSTANTS
 * ========================================================================= */

#define POOL_DEFAULT_CONNECT_TIMEOUT_MS  5000
#define POOL_HASH_BUCKETS                64u    /* Power of 2 */
#define POOL_HASH_MASK                   (POOL_HASH_BUCKETS - 1u)

/* ============================================================================
 * POOL ENTRY
 * ========================================================================= */

typedef struct pool_entry_s {
    tcp_connection_t*    conn;
    char                 host[256];
    uint16_t             port;
    uint64_t             last_used_ms;
    bool                 in_use;
    bool                 failed;

    /* Hash bucket chain (singly linked, per bucket) */
    struct pool_entry_s* bucket_next;

    /* LRU doubly-linked list (across all entries) */
    struct pool_entry_s* lru_prev;
    struct pool_entry_s* lru_next;
} pool_entry_t;

/* ============================================================================
 * POOL STRUCTURE
 * ========================================================================= */

struct tcp_pool_s {
    /* Hash table — each bucket is a head pointer to a chain of entries */
    pool_entry_t*    buckets[POOL_HASH_BUCKETS];

    /* LRU list: head = most recently used, tail = eviction candidate */
    pool_entry_t*    lru_head;
    pool_entry_t*    lru_tail;

    size_t           max_connections;
    size_t           current_size;
    int              connect_timeout_ms;
    tcp_connection_config_t conn_config;

    bool             shutting_down;
    pthread_mutex_t  lock;

    _Atomic uint64_t hits;
    _Atomic uint64_t misses;

    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           pool_size_metric;
    metric_t*           pool_hits_metric;
    metric_t*           pool_misses_metric;
};

/* ============================================================================
 * UTILITIES
 * ========================================================================= */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* djb2 hash of host XOR port */
static uint32_t host_hash(const char* host, uint16_t port) {
    uint32_t h = 5381u;
    for (const unsigned char* p = (const unsigned char*)host; *p; p++) {
        h = ((h << 5) + h) ^ (uint32_t)*p;
    }
    h ^= (uint32_t)port * 2654435761u;
    return h & POOL_HASH_MASK;
}

/* ============================================================================
 * LRU LIST OPERATIONS  (all must be called with lock held)
 * ========================================================================= */

static void lru_prepend(tcp_pool_t* pool, pool_entry_t* e) {
    e->lru_prev = NULL;
    e->lru_next = pool->lru_head;
    if (pool->lru_head) pool->lru_head->lru_prev = e;
    pool->lru_head = e;
    if (!pool->lru_tail) pool->lru_tail = e;
}

static void lru_remove(tcp_pool_t* pool, pool_entry_t* e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else             pool->lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else             pool->lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void lru_touch(tcp_pool_t* pool, pool_entry_t* e) {
    if (pool->lru_head == e) return;
    lru_remove(pool, e);
    lru_prepend(pool, e);
}

/* ============================================================================
 * HASH TABLE OPERATIONS  (all must be called with lock held)
 * ========================================================================= */

/* Unlink entry from its hash bucket chain */
static void bucket_remove(tcp_pool_t* pool, pool_entry_t* e) {
    uint32_t idx = host_hash(e->host, e->port);
    pool_entry_t** pp = &pool->buckets[idx];
    while (*pp) {
        if (*pp == e) { *pp = e->bucket_next; e->bucket_next = NULL; return; }
        pp = &(*pp)->bucket_next;
    }
}

/* Free one pool entry completely (close conn if not failed/NULL) */
static void entry_free(pool_entry_t* e) {
    if (e->conn && !e->failed) tcp_close(e->conn);
    free(e);
}

/* Evict the LRU idle entry. Returns true if one was evicted. */
static bool evict_lru_idle(tcp_pool_t* pool) {
    /* Walk LRU from tail (oldest) looking for idle+healthy */
    pool_entry_t* e = pool->lru_tail;
    while (e) {
        if (!e->in_use) {
            lru_remove(pool, e);
            bucket_remove(pool, e);
            pool->current_size--;
            if (pool->pool_size_metric)
                metrics_gauge_set(pool->pool_size_metric, (int64_t)pool->current_size);
            entry_free(e);
            return true;
        }
        e = e->lru_prev;
    }
    return false;
}

/* ============================================================================
 * tcp_pool_create_with_config
 * ========================================================================= */

distric_err_t tcp_pool_create_with_config(
    const tcp_pool_config_t* config,
    metrics_registry_t*      metrics,
    logger_t*                logger,
    tcp_pool_t**             pool_out
) {
    if (!config || !pool_out) return DISTRIC_ERR_INVALID_ARG;
    if (config->max_connections == 0) return DISTRIC_ERR_INVALID_ARG;

    tcp_pool_t* p = calloc(1, sizeof(*p));
    if (!p) return DISTRIC_ERR_ALLOC_FAILURE;

    p->max_connections     = config->max_connections;
    p->connect_timeout_ms  = config->connect_timeout_ms
                             ? config->connect_timeout_ms
                             : POOL_DEFAULT_CONNECT_TIMEOUT_MS;
    p->conn_config         = config->conn_config;
    p->metrics             = metrics;
    p->logger              = logger;
    p->shutting_down       = false;
    p->lru_head            = NULL;
    p->lru_tail            = NULL;

    atomic_init(&p->hits,   0);
    atomic_init(&p->misses, 0);
    pthread_mutex_init(&p->lock, NULL);

    if (metrics) {
        metrics_register_gauge(metrics, "tcp_pool_size",
            "TCP connection pool size", NULL, 0, &p->pool_size_metric);
        metrics_register_counter(metrics, "tcp_pool_hits_total",
            "TCP pool cache hits", NULL, 0, &p->pool_hits_metric);
        metrics_register_counter(metrics, "tcp_pool_misses_total",
            "TCP pool cache misses", NULL, 0, &p->pool_misses_metric);
    }

    if (logger) {
        char max_str[16];
        snprintf(max_str, sizeof(max_str), "%zu", config->max_connections);
        LOG_INFO(logger, "tcp_pool", "Connection pool created",
                "max_connections", max_str, NULL);
    }

    *pool_out = p;
    return DISTRIC_OK;
}

distric_err_t tcp_pool_create(
    size_t              max_connections,
    metrics_registry_t* metrics,
    logger_t*           logger,
    tcp_pool_t**        pool_out
) {
    tcp_pool_config_t cfg = {
        .max_connections    = max_connections,
        .connect_timeout_ms = POOL_DEFAULT_CONNECT_TIMEOUT_MS,
    };
    return tcp_pool_create_with_config(&cfg, metrics, logger, pool_out);
}

/* ============================================================================
 * tcp_pool_acquire — O(1) hash lookup
 * ========================================================================= */

distric_err_t tcp_pool_acquire(
    tcp_pool_t*        pool,
    const char*        host,
    uint16_t           port,
    tcp_connection_t** conn_out
) {
    if (!pool || !host || !conn_out) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&pool->lock);

    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->lock);
        return DISTRIC_ERR_SHUTDOWN;
    }

    /* O(1) bucket lookup */
    uint32_t idx = host_hash(host, port);
    pool_entry_t* e = pool->buckets[idx];
    while (e) {
        if (!e->in_use && !e->failed && e->conn &&
            e->port == port && strcmp(e->host, host) == 0)
        {
            /* Cache hit */
            e->in_use       = true;
            e->last_used_ms = now_ms();
            lru_touch(pool, e);
            *conn_out = e->conn;
            atomic_fetch_add(&pool->hits, 1);
            if (pool->pool_hits_metric) metrics_counter_inc(pool->pool_hits_metric);
            if (pool->logger) {
                char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
                LOG_DEBUG(pool->logger, "tcp_pool", "Connection reused",
                         "host", host, "port", ps, NULL);
            }
            pthread_mutex_unlock(&pool->lock);
            return DISTRIC_OK;
        }
        e = e->bucket_next;
    }

    /* Cache miss — release lock while connecting */
    atomic_fetch_add(&pool->misses, 1);
    if (pool->pool_misses_metric) metrics_counter_inc(pool->pool_misses_metric);
    pthread_mutex_unlock(&pool->lock);

    tcp_connection_t* new_conn = NULL;
    distric_err_t err = tcp_connect(
        host, port, pool->connect_timeout_ms,
        &pool->conn_config, pool->metrics, pool->logger, &new_conn);

    if (err != DISTRIC_OK || !new_conn) return err;

    pthread_mutex_lock(&pool->lock);

    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->lock);
        tcp_close(new_conn);
        return DISTRIC_ERR_SHUTDOWN;
    }

    /* Evict if at capacity */
    if (pool->current_size >= pool->max_connections) {
        evict_lru_idle(pool);
        /* If still at capacity (all in-use), just let current_size exceed limit
           rather than closing an in-use connection */
    }

    pool_entry_t* entry = calloc(1, sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&pool->lock);
        tcp_close(new_conn);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }

    entry->conn         = new_conn;
    entry->port         = port;
    entry->in_use       = true;
    entry->last_used_ms = now_ms();
    strncpy(entry->host, host, sizeof(entry->host) - 1);

    /* Insert into hash bucket */
    uint32_t bidx = host_hash(host, port);
    entry->bucket_next   = pool->buckets[bidx];
    pool->buckets[bidx]  = entry;

    /* Insert at LRU head */
    lru_prepend(pool, entry);

    pool->current_size++;
    if (pool->pool_size_metric)
        metrics_gauge_set(pool->pool_size_metric, (int64_t)pool->current_size);

    *conn_out = new_conn;
    pthread_mutex_unlock(&pool->lock);
    return DISTRIC_OK;
}

/* ============================================================================
 * tcp_pool_release
 * ========================================================================= */

void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn) {
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->lock);

    /* Find entry by connection pointer — O(n) but this is the slow path */
    for (uint32_t i = 0; i < POOL_HASH_BUCKETS; i++) {
        pool_entry_t* e = pool->buckets[i];
        while (e) {
            if (e->conn == conn) {
                e->in_use       = false;
                e->last_used_ms = now_ms();
                lru_touch(pool, e);

                /* If over capacity or failed, close immediately */
                if (e->failed || pool->current_size > pool->max_connections) {
                    lru_remove(pool, e);
                    bucket_remove(pool, e);
                    pool->current_size--;
                    if (pool->pool_size_metric)
                        metrics_gauge_set(pool->pool_size_metric,
                                          (int64_t)pool->current_size);
                    pthread_mutex_unlock(&pool->lock);
                    entry_free(e);
                    return;
                }

                pthread_mutex_unlock(&pool->lock);
                return;
            }
            e = e->bucket_next;
        }
    }

    /* Not in pool — just close */
    pthread_mutex_unlock(&pool->lock);
    tcp_close(conn);
}

/* ============================================================================
 * tcp_pool_mark_failed
 * ========================================================================= */

void tcp_pool_mark_failed(tcp_pool_t* pool, tcp_connection_t* conn) {
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->lock);
    for (uint32_t i = 0; i < POOL_HASH_BUCKETS; i++) {
        for (pool_entry_t* e = pool->buckets[i]; e; e = e->bucket_next) {
            if (e->conn == conn) {
                e->failed = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&pool->lock);
}

/* ============================================================================
 * tcp_pool_get_stats
 * ========================================================================= */

void tcp_pool_get_stats(
    tcp_pool_t* pool,
    size_t*     size_out,
    uint64_t*   hits_out,
    uint64_t*   misses_out
) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    if (size_out)   *size_out   = pool->current_size;
    if (hits_out)   *hits_out   = atomic_load(&pool->hits);
    if (misses_out) *misses_out = atomic_load(&pool->misses);
    pthread_mutex_unlock(&pool->lock);
}

/* ============================================================================
 * tcp_pool_destroy
 * ========================================================================= */

void tcp_pool_destroy(tcp_pool_t* pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = true;

    /* Wait up to 500 ms for in-use connections to be released */
    for (int retries = 0; retries < 50; retries++) {
        bool any = false;
        for (uint32_t i = 0; i < POOL_HASH_BUCKETS && !any; i++) {
            for (pool_entry_t* e = pool->buckets[i]; e; e = e->bucket_next) {
                if (e->in_use) { any = true; break; }
            }
        }
        if (!any) break;
        pthread_mutex_unlock(&pool->lock);
        usleep(10000);
        pthread_mutex_lock(&pool->lock);
    }

    /* Close all entries */
    for (uint32_t i = 0; i < POOL_HASH_BUCKETS; i++) {
        pool_entry_t* e = pool->buckets[i];
        while (e) {
            pool_entry_t* next = e->bucket_next;
            if (e->conn) tcp_close(e->conn);
            free(e);
            e = next;
        }
        pool->buckets[i] = NULL;
    }
    pool->lru_head     = NULL;
    pool->lru_tail     = NULL;
    pool->current_size = 0;

    /* NULL observability pointers */
    pool->logger  = NULL;
    pool->metrics = NULL;

    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}