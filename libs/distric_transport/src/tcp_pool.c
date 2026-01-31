//####################
// FILE: /libs/distric_transport/src/tcp_pool.c - FIXED VERSION
//####################

/**
 * @file tcp_pool.c
 * @brief TCP Connection Pool Implementation - CRITICAL FIXES
 * 
 * FIXES APPLIED:
 * 1. Validate connection is alive before reusing (send with MSG_NOSIGNAL)
 * 2. Remove invalid connections from pool immediately
 * 3. Better error handling to prevent use-after-free
 * 4. Mark connections as invalid when pool is full instead of closing immediately
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_transport/tcp_pool.h"
#include <distric_obs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/socket.h>
#include <errno.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================= */

typedef struct pool_entry_s {
    tcp_connection_t* conn;
    char host[256];
    uint16_t port;
    uint64_t last_used;
    bool in_use;
    bool valid;
    struct pool_entry_s* next;
} pool_entry_t;

struct tcp_pool_s {
    pool_entry_t* entries;
    size_t max_connections;
    size_t current_size;
    
    _Atomic uint64_t hits;
    _Atomic uint64_t misses;
    
    pthread_mutex_t lock;

    metrics_registry_t* metrics;
    logger_t* logger;
    
    metric_t* pool_size_metric;
    metric_t* pool_hits_metric;
    metric_t* pool_misses_metric;
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* CRITICAL FIX: Test if connection is still alive */
static bool is_connection_alive(tcp_connection_t* conn) {
    if (!conn) return false;
    
    /* Try to peek at the socket to see if it's still connected */
    char buf[1];
    ssize_t result = recv(tcp_get_connection_id(conn), buf, sizeof(buf), 
                         MSG_PEEK | MSG_DONTWAIT);
    
    if (result == 0) {
        /* Connection closed by peer */
        return false;
    }
    
    if (result < 0) {
        /* Check error */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No data available, but connection is alive */
            return true;
        }
        /* Other errors mean connection is dead */
        return false;
    }
    
    /* Data available, connection is alive */
    return true;
}

static pool_entry_t* find_entry(tcp_pool_t* pool, const char* host, uint16_t port) {
    if (!pool || !host) return NULL;
    
    pool_entry_t* entry = pool->entries;
    while (entry) {
        if (entry->valid && !entry->in_use && 
            entry->port == port && 
            strcmp(entry->host, host) == 0) {
            
            /* CRITICAL FIX: Validate connection is still alive */
            if (!is_connection_alive(entry->conn)) {
                /* Connection is dead, mark invalid */
                entry->valid = false;
                entry = entry->next;
                continue;
            }
            
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/* CRITICAL FIX: Remove invalid entries from pool */
static void cleanup_invalid_entries(tcp_pool_t* pool) {
    pool_entry_t** current = &pool->entries;
    
    while (*current) {
        pool_entry_t* entry = *current;
        
        if (!entry->valid && !entry->in_use) {
            /* Remove from list */
            *current = entry->next;
            
            /* Close connection */
            if (entry->conn) {
                tcp_close(entry->conn);
            }
            
            free(entry);
            pool->current_size--;
        } else {
            current = &entry->next;
        }
    }
}

/* ============================================================================
 * TCP POOL IMPLEMENTATION
 * ========================================================================= */

distric_err_t tcp_pool_create(
    size_t max_connections,
    metrics_registry_t* metrics,
    logger_t* logger,
    tcp_pool_t** pool
) {
    if (!pool || max_connections == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tcp_pool_t* p = calloc(1, sizeof(tcp_pool_t));
    if (!p) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    p->max_connections = max_connections;
    p->current_size = 0;
    p->entries = NULL;
    p->metrics = metrics;
    p->logger = logger;
    
    atomic_init(&p->hits, 0);
    atomic_init(&p->misses, 0);
    
    pthread_mutex_init(&p->lock, NULL);
    
    /* Register metrics */
    if (metrics) {
        metrics_register_gauge(metrics, "tcp_pool_size",
                              "TCP connection pool size", NULL, 0, &p->pool_size_metric);
        metrics_register_counter(metrics, "tcp_pool_hits_total",
                                "TCP pool cache hits", NULL, 0, &p->pool_hits_metric);
        metrics_register_counter(metrics, "tcp_pool_misses_total",
                                "TCP pool cache misses", NULL, 0, &p->pool_misses_metric);
    }
    
    if (logger) {
        char max_str[32];
        snprintf(max_str, sizeof(max_str), "%zu", max_connections);
        
        LOG_INFO(logger, "tcp_pool", "Connection pool created",
                "max_connections", max_str, NULL);
    }
    
    *pool = p;
    return DISTRIC_OK;
}

distric_err_t tcp_pool_acquire(
    tcp_pool_t* pool,
    const char* host,
    uint16_t port,
    tcp_connection_t** conn
) {
    if (!pool || !host || !conn) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    /* CRITICAL FIX: Clean up invalid entries first */
    cleanup_invalid_entries(pool);
    
    /* Try to find existing valid connection */
    pool_entry_t* entry = find_entry(pool, host, port);
    if (entry && entry->valid) {
        /* Cache hit */
        entry->in_use = true;
        entry->last_used = get_timestamp_ms();
        *conn = entry->conn;
        
        atomic_fetch_add(&pool->hits, 1);
        if (pool->pool_hits_metric) {
            metrics_counter_inc(pool->pool_hits_metric);
        }
        
        if (pool->logger) {
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%u", port);
            
            LOG_DEBUG(pool->logger, "tcp_pool", "Connection reused",
                     "host", host,
                     "port", port_str, NULL);
        }
        
        pthread_mutex_unlock(&pool->lock);
        return DISTRIC_OK;
    }
    
    /* Cache miss - create new connection */
    atomic_fetch_add(&pool->misses, 1);
    if (pool->pool_misses_metric) {
        metrics_counter_inc(pool->pool_misses_metric);
    }
    
    pthread_mutex_unlock(&pool->lock);

    /* Connect (outside lock to avoid blocking other threads) */
    tcp_connection_t* new_conn;
    distric_err_t err = tcp_connect(host, port, 5000, pool->metrics, pool->logger, &new_conn);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    if (!new_conn) {
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    /* Create new entry */
    pool_entry_t* new_entry = calloc(1, sizeof(pool_entry_t));
    if (!new_entry) {
        pthread_mutex_unlock(&pool->lock);
        tcp_close(new_conn);
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    new_entry->conn = new_conn;
    strncpy(new_entry->host, host, sizeof(new_entry->host) - 1);
    new_entry->host[sizeof(new_entry->host) - 1] = '\0';
    new_entry->port = port;
    new_entry->last_used = get_timestamp_ms();
    new_entry->in_use = true;
    new_entry->valid = true;
    new_entry->next = pool->entries;
    
    pool->entries = new_entry;
    pool->current_size++;

    if (pool->pool_size_metric) {
        metrics_gauge_set(pool->pool_size_metric, pool->current_size);
    }
    
    if (pool->logger) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        char size_str[32];
        snprintf(size_str, sizeof(size_str), "%zu", pool->current_size);
        
        LOG_DEBUG(pool->logger, "tcp_pool", "New connection created",
                 "host", host,
                 "port", port_str,
                 "pool_size", size_str, NULL);
    }
    
    *conn = new_conn;
    pthread_mutex_unlock(&pool->lock);
    
    return DISTRIC_OK;
}

void tcp_pool_release(tcp_pool_t* pool, tcp_connection_t* conn) {
    if (!pool || !conn) return;
    
    pthread_mutex_lock(&pool->lock);

    /* Find entry for this connection */
    pool_entry_t* entry = pool->entries;
    
    while (entry) {
        if (entry->conn == conn) {
            /* CRITICAL FIX: Check if connection is still alive */
            if (!is_connection_alive(conn)) {
                /* Connection is dead, mark invalid */
                entry->valid = false;
                entry->in_use = false;
                pthread_mutex_unlock(&pool->lock);
                return;
            }
            
            if (pool->current_size > pool->max_connections) {
                /* Pool is full, mark as invalid */
                entry->valid = false;
                entry->in_use = false;
            } else {
                /* Return to pool */
                entry->in_use = false;
                entry->last_used = get_timestamp_ms();
            }
            
            pthread_mutex_unlock(&pool->lock);
            return;
        }
        
        entry = entry->next;
    }
    
    /* Connection not found in pool - just close it */
    pthread_mutex_unlock(&pool->lock);
    
    if (pool->logger) {
        LOG_WARN(pool->logger, "tcp_pool", 
                "Released connection not found in pool", NULL);
    }
    
    tcp_close(conn);
}

void tcp_pool_get_stats(
    tcp_pool_t* pool,
    size_t* size_out,
    uint64_t* hits_out,
    uint64_t* misses_out
) {
    if (!pool) return;
    
    pthread_mutex_lock(&pool->lock);
    
    if (size_out) *size_out = pool->current_size;
    if (hits_out) *hits_out = atomic_load(&pool->hits);
    if (misses_out) *misses_out = atomic_load(&pool->misses);
    
    pthread_mutex_unlock(&pool->lock);
}

void tcp_pool_destroy(tcp_pool_t* pool) {
    if (!pool) return;
    
    pthread_mutex_lock(&pool->lock);

    /* Close all connections */
    pool_entry_t* entry = pool->entries;
    while (entry) {
        pool_entry_t* next = entry->next;
        
        /* Close connection if not in use */
        if (entry->conn && !entry->in_use) {
            tcp_close(entry->conn);
        }
        
        free(entry);
        entry = next;
    }
    
    if (pool->logger) {
        LOG_INFO(pool->logger, "tcp_pool", "Connection pool destroyed", NULL);
    }
    
    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    
    free(pool);
}