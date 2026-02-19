/**
 * @file circuit_breaker.c
 * @brief Per-host circuit breaker implementation — v2
 *
 * Changes from v1:
 *
 * LOCK STRIPING (Item 3 from gap analysis)
 * ----------------------------------------
 * The global pthread_mutex_t has been replaced with a pthread_rwlock_t.
 *
 * Rationale:
 *   The dominant operation in production is cb_is_allowed() with state ==
 *   CB_STATE_CLOSED (the circuit is closed for >99.9% of hosts at steady
 *   state). Under 10k+ concurrent connections this path was serialised by
 *   the global mutex.
 *
 * New scheme:
 *   - Read lock (pthread_rwlock_rdlock) for cb_is_allowed() CLOSED fast path
 *     and cb_get_state(). Multiple callers execute concurrently.
 *   - Write lock (pthread_rwlock_wrlock) for:
 *       * cb_record_failure() and cb_record_success() (modify entry + LRU).
 *       * The OPEN/HALF_OPEN transition inside cb_is_allowed() (rare path).
 *   - Double-check pattern in cb_is_allowed(): take rdlock first, read state
 *     atomically; if CLOSED return under rdlock. Only re-acquire wrlock for
 *     the state-changing paths (OPEN → HALF_OPEN).
 *
 * This eliminates lock contention for the common CLOSED case while keeping
 * correct serialisation for all mutating paths.
 *
 * Per-entry atomics (state, failure_count) remain, providing fine-grained
 * visibility without additional locking on the read path.
 *
 * Registry: fixed-size hash table (open addressing, linear probing).
 *           LRU eviction when full.
 *
 * State word layout: see cb_state_t.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "circuit_breaker.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* ============================================================================
 * CONSTANTS
 * ========================================================================= */

#define CB_DEFAULT_FAILURE_THRESHOLD  5u
#define CB_DEFAULT_RECOVERY_MS        5000u
#define CB_DEFAULT_WINDOW_MS          10000u
#define CB_DEFAULT_MAX_ENTRIES        1024u
#define CB_MAX_RECOVERY_MS            60000u
#define CB_TABLE_LOAD_FACTOR          2u

/* ============================================================================
 * PER-ENTRY STRUCTURE
 * ========================================================================= */

typedef struct {
    char     host[256];
    uint16_t port;
    bool     occupied;

    _Atomic uint32_t state;
    _Atomic uint32_t failure_count;

    int64_t  last_failure_ns;
    int64_t  open_until_ns;
    uint32_t recovery_ms;
    bool     probe_in_flight;

    /* LRU chain */
    int lru_prev;
    int lru_next;
} cb_entry_t;

/* ============================================================================
 * REGISTRY STRUCTURE
 * ========================================================================= */

struct cb_registry_s {
    cb_entry_t*     table;
    uint32_t        table_size;
    uint32_t        occupied_count;
    uint32_t        max_entries;

    uint32_t        failure_threshold;
    uint32_t        base_recovery_ms;
    uint32_t        window_ms;

    int             lru_head;
    int             lru_tail;

    /*
     * rwlock replaces the former global mutex.
     *
     * Read lock:  concurrent cb_is_allowed() CLOSED path, cb_get_state().
     * Write lock: all state-mutating paths (record_failure, record_success,
     *             insert/evict, OPEN→HALF_OPEN transition).
     */
    pthread_rwlock_t rwlock;

    metrics_registry_t* metrics;
    logger_t*           logger;
    metric_t*           open_total_metric;
    metric_t*           half_open_metric;
    metric_t*           rejected_metric;
};

/* ============================================================================
 * TIME HELPERS
 * ========================================================================= */

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t ms_to_ns(uint32_t ms) {
    return (int64_t)ms * 1000000LL;
}

/* ============================================================================
 * HASH / PROBE
 * ========================================================================= */

static uint32_t entry_hash(const char* host, uint16_t port) {
    uint32_t h = 5381u;
    for (const unsigned char* p = (const unsigned char*)host; *p; p++) {
        h = ((h << 5) + h) ^ (uint32_t)*p;
    }
    h ^= (uint32_t)port * 2654435761u;
    return h;
}

/* Find slot index. Returns index or -1 if not found. Must hold at least rdlock. */
static int table_find(const cb_registry_t* reg, const char* host, uint16_t port) {
    uint32_t mask = reg->table_size - 1;
    uint32_t idx  = entry_hash(host, port) & mask;

    for (uint32_t probe = 0; probe < reg->table_size; probe++) {
        const cb_entry_t* e = &reg->table[idx];
        if (!e->occupied)                                    return -1;
        if (e->port == port && strcmp(e->host, host) == 0)  return (int)idx;
        idx = (idx + 1) & mask;
    }
    return -1;
}

/* Find or insert. Returns index or -1. Must hold wrlock. */
static int table_find_or_insert(cb_registry_t* reg, const char* host, uint16_t port) {
    uint32_t mask     = reg->table_size - 1;
    uint32_t hash_idx = entry_hash(host, port) & mask;
    uint32_t idx      = hash_idx;

    /* Linear probe: look for existing or first empty slot */
    int empty_slot = -1;
    for (uint32_t probe = 0; probe < reg->table_size; probe++) {
        cb_entry_t* e = &reg->table[idx];
        if (e->occupied && e->port == port && strcmp(e->host, host) == 0) {
            return (int)idx;
        }
        if (!e->occupied && empty_slot < 0) {
            empty_slot = (int)idx;
        }
        idx = (idx + 1) & mask;
    }

    if (empty_slot >= 0 && reg->occupied_count < reg->max_entries) {
        /* Insert into empty slot */
        cb_entry_t* e = &reg->table[empty_slot];
        strncpy(e->host, host, sizeof(e->host) - 1);
        e->port            = port;
        e->occupied        = true;
        e->probe_in_flight = false;
        e->last_failure_ns = 0;
        e->open_until_ns   = 0;
        e->recovery_ms     = reg->base_recovery_ms;
        e->lru_prev        = -1;
        e->lru_next        = -1;
        atomic_store(&e->state,         CB_STATE_CLOSED);
        atomic_store(&e->failure_count, 0);
        reg->occupied_count++;
        return empty_slot;
    }

    /* Table full (max_entries reached): evict LRU tail */
    if (reg->lru_tail < 0) return -1;

    int evict_idx   = reg->lru_tail;
    cb_entry_t* old = &reg->table[evict_idx];

    /* Unlink from LRU */
    if (old->lru_prev >= 0)
        reg->table[old->lru_prev].lru_next = -1;
    else
        reg->lru_head = -1;
    reg->lru_tail = old->lru_prev;

    /* Re-initialise slot for new entry */
    memset(old->host, 0, sizeof(old->host));
    strncpy(old->host, host, sizeof(old->host) - 1);
    old->port            = port;
    old->occupied        = true;
    old->probe_in_flight = false;
    old->last_failure_ns = 0;
    old->open_until_ns   = 0;
    old->recovery_ms     = reg->base_recovery_ms;
    old->lru_prev        = -1;
    old->lru_next        = -1;
    atomic_store(&old->state,         CB_STATE_CLOSED);
    atomic_store(&old->failure_count, 0);

    return evict_idx;
}

/* ============================================================================
 * LRU HELPERS — must hold wrlock
 * ========================================================================= */

static void lru_touch(cb_registry_t* reg, int idx) {
    if (idx < 0) return;
    cb_entry_t* e = &reg->table[idx];

    /* Unlink from current position */
    if (e->lru_prev >= 0) reg->table[e->lru_prev].lru_next = e->lru_next;
    else reg->lru_head = e->lru_next;

    if (e->lru_next >= 0) reg->table[e->lru_next].lru_prev = e->lru_prev;
    else reg->lru_tail = e->lru_prev;

    /* Move to head */
    e->lru_prev = -1;
    e->lru_next = reg->lru_head;
    if (reg->lru_head >= 0) reg->table[reg->lru_head].lru_prev = idx;
    reg->lru_head = idx;
    if (reg->lru_tail < 0) reg->lru_tail = idx;
}

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

int cb_registry_create(
    const cb_config_t*   cfg,
    metrics_registry_t*  metrics,
    logger_t*            logger,
    cb_registry_t**      out
) {
    if (!out) return -1;

    cb_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) return -1;

    reg->failure_threshold = (cfg && cfg->failure_threshold) ? cfg->failure_threshold : CB_DEFAULT_FAILURE_THRESHOLD;
    reg->base_recovery_ms  = (cfg && cfg->recovery_ms)       ? cfg->recovery_ms       : CB_DEFAULT_RECOVERY_MS;
    reg->window_ms         = (cfg && cfg->window_ms)         ? cfg->window_ms         : CB_DEFAULT_WINDOW_MS;
    reg->max_entries       = (cfg && cfg->max_entries)       ? cfg->max_entries       : CB_DEFAULT_MAX_ENTRIES;

    uint32_t sz = 1;
    while (sz < reg->max_entries * CB_TABLE_LOAD_FACTOR) sz <<= 1;
    reg->table_size = sz;

    reg->table = calloc(sz, sizeof(cb_entry_t));
    if (!reg->table) { free(reg); return -1; }

    for (uint32_t i = 0; i < sz; i++) {
        reg->table[i].lru_prev = -1;
        reg->table[i].lru_next = -1;
    }
    reg->lru_head = -1;
    reg->lru_tail = -1;

    reg->metrics = metrics;
    reg->logger  = logger;

    pthread_rwlock_init(&reg->rwlock, NULL);

    if (metrics) {
        metrics_register_counter(metrics, "cb_open_transitions_total",
            "Circuit breaker OPEN transitions", NULL, 0, &reg->open_total_metric);
        metrics_register_counter(metrics, "cb_rejected_connections_total",
            "Connections rejected by open circuit", NULL, 0, &reg->rejected_metric);
    }

    *out = reg;
    return 0;
}

void cb_registry_destroy(cb_registry_t* reg) {
    if (!reg) return;
    pthread_rwlock_wrlock(&reg->rwlock);
    free(reg->table);
    reg->table = NULL;
    pthread_rwlock_unlock(&reg->rwlock);
    pthread_rwlock_destroy(&reg->rwlock);
    free(reg);
}

/* ============================================================================
 * cb_is_allowed
 *
 * Fast path (CLOSED): read lock only. No writers blocked.
 * Slow path (OPEN/HALF_OPEN): upgrade to write lock for state transition.
 *
 * Double-check pattern:
 *   1. rdlock → find entry → read state atomically.
 *   2. If CLOSED → unlock, return true.  (No write needed.)
 *   3. Else unlock rdlock → wrlock → re-find → handle transition.
 * ========================================================================= */

bool cb_is_allowed(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return true;

    /* ---- Fast path: read lock ------------------------------------------- */
    pthread_rwlock_rdlock(&reg->rwlock);
    int idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_rwlock_unlock(&reg->rwlock);
        return true;  /* Unknown host → allow */
    }

    uint32_t st = atomic_load_explicit(&reg->table[idx].state, memory_order_acquire);
    if (st == CB_STATE_CLOSED) {
        /* Most common case: no modification needed, no writer blocked. */
        pthread_rwlock_unlock(&reg->rwlock);
        return true;
    }
    pthread_rwlock_unlock(&reg->rwlock);

    /* ---- Slow path: write lock (OPEN or HALF_OPEN) ----------------------- */
    pthread_rwlock_wrlock(&reg->rwlock);

    /* Re-find: table could have been modified between unlock and wrlock. */
    idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_rwlock_unlock(&reg->rwlock);
        return true;
    }

    cb_entry_t* e   = &reg->table[idx];
    st              = atomic_load(&e->state);
    int64_t     now = mono_ns();

    if (st == CB_STATE_CLOSED) {
        /* Race: another thread closed it between our rdlock release and wrlock. */
        lru_touch(reg, idx);
        pthread_rwlock_unlock(&reg->rwlock);
        return true;
    }

    if (st == CB_STATE_OPEN) {
        if (now >= e->open_until_ns && !e->probe_in_flight) {
            atomic_store(&e->state, CB_STATE_HALF_OPEN);
            e->probe_in_flight = true;
            lru_touch(reg, idx);
            pthread_rwlock_unlock(&reg->rwlock);
            if (reg->logger) {
                LOG_INFO(reg->logger, "circuit_breaker",
                        "Probe allowed (OPEN → HALF_OPEN)", "host", host, NULL);
            }
            return true;
        }
        if (reg->rejected_metric) metrics_counter_inc(reg->rejected_metric);
        pthread_rwlock_unlock(&reg->rwlock);
        return false;
    }

    /* HALF_OPEN — probe already in flight */
    if (e->probe_in_flight) {
        if (reg->rejected_metric) metrics_counter_inc(reg->rejected_metric);
        pthread_rwlock_unlock(&reg->rwlock);
        return false;
    }

    pthread_rwlock_unlock(&reg->rwlock);
    return true;
}

/* ============================================================================
 * cb_record_success
 * ========================================================================= */

void cb_record_success(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return;

    pthread_rwlock_wrlock(&reg->rwlock);
    int idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_rwlock_unlock(&reg->rwlock);
        return;
    }

    cb_entry_t* e  = &reg->table[idx];
    uint32_t    st = atomic_load(&e->state);

    atomic_store(&e->failure_count, 0);
    atomic_store(&e->state, CB_STATE_CLOSED);
    e->probe_in_flight = false;
    e->recovery_ms     = reg->base_recovery_ms;
    lru_touch(reg, idx);
    pthread_rwlock_unlock(&reg->rwlock);

    if (st != CB_STATE_CLOSED && reg->logger) {
        LOG_INFO(reg->logger, "circuit_breaker",
                "Circuit CLOSED after success", "host", host, NULL);
    }
}

/* ============================================================================
 * cb_record_failure
 * ========================================================================= */

void cb_record_failure(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return;

    pthread_rwlock_wrlock(&reg->rwlock);
    int idx = table_find_or_insert(reg, host, port);
    if (idx < 0) {
        pthread_rwlock_unlock(&reg->rwlock);
        return;
    }

    cb_entry_t* e      = &reg->table[idx];
    int64_t     now    = mono_ns();
    uint32_t    st     = atomic_load(&e->state);

    if ((now - e->last_failure_ns) > ms_to_ns(reg->window_ms)) {
        atomic_store(&e->failure_count, 0);
    }
    e->last_failure_ns = now;

    uint32_t failures = atomic_fetch_add(&e->failure_count, 1) + 1;

    if (st == CB_STATE_HALF_OPEN || failures >= reg->failure_threshold) {
        e->open_until_ns   = now + ms_to_ns(e->recovery_ms);
        e->probe_in_flight = false;
        atomic_store(&e->state, CB_STATE_OPEN);
        atomic_store(&e->failure_count, 0);

        uint32_t next = e->recovery_ms * 2;
        e->recovery_ms = (next < CB_MAX_RECOVERY_MS) ? next : CB_MAX_RECOVERY_MS;

        if (reg->open_total_metric) metrics_counter_inc(reg->open_total_metric);
        if (reg->logger) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u ms", e->recovery_ms / 2);
            LOG_WARN(reg->logger, "circuit_breaker",
                    "Circuit OPEN", "host", host, "recovery_ms", buf, NULL);
        }
    }

    lru_touch(reg, idx);
    pthread_rwlock_unlock(&reg->rwlock);
}

/* ============================================================================
 * cb_get_state
 * ========================================================================= */

cb_state_t cb_get_state(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return CB_STATE_CLOSED;

    pthread_rwlock_rdlock(&reg->rwlock);
    int idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_rwlock_unlock(&reg->rwlock);
        return CB_STATE_CLOSED;
    }
    cb_state_t st = (cb_state_t)atomic_load(&reg->table[idx].state);
    pthread_rwlock_unlock(&reg->rwlock);
    return st;
}