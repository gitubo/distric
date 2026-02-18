/**
 * @file tcp_pool.c
 * @brief TCP Connection Pool Implementation v2
 *
 * Changes from v1:
 *  - Uses updated tcp_connect() signature (config parameter added).
 *  - Removed _Atomic fields from pool_entry_t; all access serialised by pool->lock.
 *  - Simplified validity tracking: a bool 'failed' flag replaces dual atomic flags.
 *  - Shutdown flag prevents new acquisitions during destroy().
 *  - Cleaner destroy(): waits briefly for in-flight releases, then closes.
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
 * DEFAULT CONSTANTS
 * ========================================================================= */

#define POOL_DEFAULT_CONNECT_TIMEOUT_MS   5000

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

typedef struct pool_entry_s {
    tcp_connection_t*   conn;
    char                host[256];
    uint16_t            port;
    uint64_t            last_used_ms;
    bool                in_use;   /* True while held by caller */
    bool                failed;   /* True if caller marked it failed */
    struct pool_entry_s* next;
} pool_entry_t;

struct tcp_pool_s {
    pool_entry_t*       entries;
    size_t              max_connections;
    size_t              current_size;
    int                 connect_timeout_ms;
    tcp_connection_config_t conn_config;

    bool                shutting_down;
    pthread_mutex_t     lock;

    _Atomic uint64_t    hits;
    _Atomic uint64_t    misses;

    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           pool_size_metric;
    metric_t*           pool_hits_metric;
    metric_t*           pool_misses_metric;
};

/* ============================================================================
 * UTILITY
 * ========================================================================= */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/** Count entries that are idle and healthy. Must be called with lock held. */
static size_t count_idle_healthy(tcp_pool_t* pool) {
    size_t count = 0;
    for (pool_entry_t* e = pool->entries; e; e = e->next) {
        if (!e->in_use && !e->failed && e->conn) count++;
    }
    return count;
}

/** Remove and free all entries that are idle AND (failed OR conn==NULL). */
static void purge_dead_entries(tcp_pool_t* pool) {
    pool_entry_t** cur = &pool->entries;
    while (*cur) {
        pool_entry_t* e = *cur;
        if (!e->in_use && (e->failed || !e->conn)) {
            *cur = e->next;
            if (e->conn) { tcp_close(e->conn); }
            free(e);
            if (pool->current_size > 0) pool->current_size--;
        } else {
            cur = &e->next;
        }
    }
}

/* ============================================================================
 * tcp_pool_create / tcp_pool_create_with_config
 * ========================================================================= */

distric_err_t tcp_pool_create_with_config(
    const tcp_pool_config_t* config,
    metrics_registry_t*      metrics,
    logger_t*                logger,
    tcp_pool_t**             pool_out
) {
    if (!config || config->max_connections == 0 || !pool_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }

    tcp_pool_t* p = calloc(1, sizeof(*p));
    if (!p) return DISTRIC_ERR_ALLOC_FAILURE;

    p->max_connections     = config->max_connections;
    p->connect_timeout_ms  = config->connect_timeout_ms > 0
                             ? config->connect_timeout_ms
                             : POOL_DEFAULT_CONNECT_TIMEOUT_MS;
    p->conn_config         = config->conn_config;
    p->metrics             = metrics;
    p->logger              = logger;
    p->shutting_down       = false;

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
 * tcp_pool_acquire
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

    /* Evict dead entries before searching */
    purge_dead_entries(pool);

    /* Search for idle healthy connection matching host:port */
    for (pool_entry_t* e = pool->entries; e; e = e->next) {
        if (e->in_use || e->failed || !e->conn) continue;
        if (e->port != port)                    continue;
        if (strcmp(e->host, host) != 0)         continue;

        /* Cache hit */
        e->in_use       = true;
        e->last_used_ms = now_ms();
        *conn_out       = e->conn;

        atomic_fetch_add(&pool->hits, 1);
        if (pool->pool_hits_metric) metrics_counter_inc(pool->pool_hits_metric);

        if (pool->logger) {
            char port_str[8];
            snprintf(port_str, sizeof(port_str), "%u", port);
            LOG_DEBUG(pool->logger, "tcp_pool", "Connection reused",
                     "host", host, "port", port_str, NULL);
        }

        pthread_mutex_unlock(&pool->lock);
        return DISTRIC_OK;
    }

    /* Cache miss: release lock while establishing new connection */
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

    entry->next    = pool->entries;
    pool->entries  = entry;
    pool->current_size++;

    if (pool->pool_size_metric)
        metrics_gauge_set(pool->pool_size_metric, (double)pool->current_size);

    if (pool->logger) {
        char port_str[8], size_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        snprintf(size_str, sizeof(size_str), "%zu", pool->current_size);
        LOG_DEBUG(pool->logger, "tcp_pool", "New connection created",
                 "host", host, "port", port_str,
                 "pool_size", size_str, NULL);
    }

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

    pool_entry_t* found = NULL;
    for (pool_entry_t* e = pool->entries; e; e = e->next) {
        if (e->conn == conn) { found = e; break; }
    }

    if (!found) {
        /* Connection not in pool (orphan) — just close it */
        pthread_mutex_unlock(&pool->lock);
        tcp_close(conn);
        return;
    }

    found->in_use = false;

    if (found->failed || pool->shutting_down) {
        /* Close invalid or unwanted connection */
        found->conn = NULL;
        tcp_close(conn);
        purge_dead_entries(pool);
    } else {
        /* Keep in pool if below capacity */
        size_t idle = count_idle_healthy(pool);
        if (idle > pool->max_connections) {
            /* Pool overfull: evict this entry */
            found->failed = true;
            found->conn   = NULL;
            tcp_close(conn);
            purge_dead_entries(pool);
        }
    }

    if (pool->pool_size_metric)
        metrics_gauge_set(pool->pool_size_metric, (double)pool->current_size);

    pthread_mutex_unlock(&pool->lock);
}

/* ============================================================================
 * tcp_pool_mark_failed
 * ========================================================================= */

void tcp_pool_mark_failed(tcp_pool_t* pool, tcp_connection_t* conn) {
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->lock);
    for (pool_entry_t* e = pool->entries; e; e = e->next) {
        if (e->conn == conn) {
            e->failed = true;
            break;
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
    if (size_out)   *size_out   = count_idle_healthy(pool);
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

    /* Wait up to 500ms for in-use connections to be released */
    for (int retries = 0; retries < 50; retries++) {
        bool any_in_use = false;
        for (pool_entry_t* e = pool->entries; e; e = e->next) {
            if (e->in_use) { any_in_use = true; break; }
        }
        if (!any_in_use) break;

        pthread_mutex_unlock(&pool->lock);
        usleep(10000);  /* 10ms */
        pthread_mutex_lock(&pool->lock);
    }

    /* Close all remaining entries */
    pool_entry_t* e = pool->entries;
    while (e) {
        pool_entry_t* next = e->next;
        if (e->conn) tcp_close(e->conn);
        free(e);
        e = next;
    }
    pool->entries      = NULL;
    pool->current_size = 0;

    if (pool->logger)
        LOG_INFO(pool->logger, "tcp_pool", "Connection pool destroyed", NULL);

    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}