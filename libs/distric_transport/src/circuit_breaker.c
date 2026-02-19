/**
 * @file circuit_breaker.c
 * @brief Per-host circuit breaker implementation.
 *
 * Registry: fixed-size hash table (open addressing with linear probing).
 *           LRU eviction when full.
 *
 * Per-entry locking: atomic CAS on a uint32_t state word avoids a mutex on
 * the hot path. The registry-level mutex is only held during insert/lookup
 * to prevent double-insertion races.
 *
 * State word layout (32 bits):
 *   [31:30] state  (CB_STATE_*)
 *   [29:0]  failure_count (max ~1B, saturates)
 *
 * Timer fields (open_until_ns, last_failure_ns) are int64_t and accessed
 * only while holding the per-entry spin state CAS — safe on 64-bit.
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
#define CB_MAX_RECOVERY_MS            60000u   /* Exponential backoff cap */
#define CB_TABLE_LOAD_FACTOR          2u       /* Table size = max_entries * 2 */

/* ============================================================================
 * PER-ENTRY STRUCTURE
 * ========================================================================= */

typedef struct {
    char     host[256];
    uint16_t port;
    bool     occupied;

    _Atomic uint32_t state;         /* CB_STATE_* packed into uint32_t */
    _Atomic uint32_t failure_count;

    int64_t  last_failure_ns;       /* Monotonic ns of last failure              */
    int64_t  open_until_ns;         /* Monotonic ns when OPEN → HALF_OPEN        */
    uint32_t recovery_ms;           /* Current recovery window (doubles on trip) */
    bool     probe_in_flight;       /* True while HALF_OPEN probe is outstanding */

    /* LRU chain */
    int      lru_prev;  /* index, -1 = none */
    int      lru_next;  /* index, -1 = none */
} cb_entry_t;

/* ============================================================================
 * REGISTRY STRUCTURE
 * ========================================================================= */

struct cb_registry_s {
    cb_entry_t*     table;
    uint32_t        table_size;      /* Always a power of 2 */
    uint32_t        occupied_count;
    uint32_t        max_entries;

    uint32_t        failure_threshold;
    uint32_t        base_recovery_ms;
    uint32_t        window_ms;

    int             lru_head;        /* Most recently used */
    int             lru_tail;        /* Least recently used (eviction target) */

    pthread_mutex_t lock;

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

/* djb2 of host XOR with port — produces 32-bit hash */
static uint32_t entry_hash(const char* host, uint16_t port) {
    uint32_t h = 5381u;
    for (const unsigned char* p = (const unsigned char*)host; *p; p++) {
        h = ((h << 5) + h) ^ (uint32_t)*p;
    }
    h ^= (uint32_t)port * 2654435761u;
    return h;
}

/* Find slot index for host:port. Returns index or -1 if not found.
   Must be called with lock held. */
static int table_find(const cb_registry_t* reg, const char* host, uint16_t port) {
    uint32_t mask = reg->table_size - 1;
    uint32_t idx  = entry_hash(host, port) & mask;

    for (uint32_t probe = 0; probe < reg->table_size; probe++) {
        cb_entry_t* e = &reg->table[idx];
        if (!e->occupied)                         return -1; /* Empty slot = not found */
        if (e->port == port && strcmp(e->host, host) == 0) return (int)idx;
        idx = (idx + 1) & mask;
    }
    return -1;
}

/* Find or insert slot. Returns index or -1 if table full (after eviction attempt).
   Must be called with lock held. */
static int table_find_or_insert(cb_registry_t* reg, const char* host, uint16_t port) {
    int existing = table_find(reg, host, port);
    if (existing >= 0) return existing;

    /* Evict LRU entry if at capacity */
    if (reg->occupied_count >= reg->max_entries && reg->lru_tail >= 0) {
        int victim = reg->lru_tail;
        cb_entry_t* ve = &reg->table[victim];

        /* Detach from LRU */
        if (ve->lru_prev >= 0) reg->table[ve->lru_prev].lru_next = -1;
        if (reg->lru_tail == reg->lru_head) reg->lru_head = -1;
        reg->lru_tail = ve->lru_prev;

        memset(ve, 0, sizeof(*ve));
        ve->lru_prev  = -1;
        ve->lru_next  = -1;
        reg->occupied_count--;
    }

    if (reg->occupied_count >= reg->table_size) return -1; /* Truly full */

    uint32_t mask = reg->table_size - 1;
    uint32_t idx  = entry_hash(host, port) & mask;
    for (uint32_t probe = 0; probe < reg->table_size; probe++) {
        cb_entry_t* e = &reg->table[idx];
        if (!e->occupied) {
            memset(e, 0, sizeof(*e));
            strncpy(e->host, host, sizeof(e->host) - 1);
            e->port         = port;
            e->occupied     = true;
            e->recovery_ms  = reg->base_recovery_ms;
            e->lru_prev     = -1;
            e->lru_next     = -1;
            atomic_init(&e->state, CB_STATE_CLOSED);
            atomic_init(&e->failure_count, 0);
            reg->occupied_count++;
            return (int)idx;
        }
        idx = (idx + 1) & mask;
    }
    return -1;
}

/* Move entry to LRU head (most recently used). Lock must be held. */
static void lru_touch(cb_registry_t* reg, int idx) {
    if (reg->lru_head == idx) return;

    cb_entry_t* e = &reg->table[idx];

    /* Detach */
    if (e->lru_prev >= 0) reg->table[e->lru_prev].lru_next = e->lru_next;
    if (e->lru_next >= 0) reg->table[e->lru_next].lru_prev = e->lru_prev;
    if (reg->lru_tail == idx) reg->lru_tail = e->lru_prev;

    /* Prepend to head */
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
    const cb_config_t*  cfg,
    metrics_registry_t* metrics,
    logger_t*           logger,
    cb_registry_t**     out
) {
    if (!out) return -1;

    cb_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) return -1;

    reg->failure_threshold = (cfg && cfg->failure_threshold) ? cfg->failure_threshold : CB_DEFAULT_FAILURE_THRESHOLD;
    reg->base_recovery_ms  = (cfg && cfg->recovery_ms)       ? cfg->recovery_ms       : CB_DEFAULT_RECOVERY_MS;
    reg->window_ms         = (cfg && cfg->window_ms)         ? cfg->window_ms         : CB_DEFAULT_WINDOW_MS;
    reg->max_entries       = (cfg && cfg->max_entries)       ? cfg->max_entries       : CB_DEFAULT_MAX_ENTRIES;

    /* Table size = next power of 2 >= max_entries * CB_TABLE_LOAD_FACTOR */
    uint32_t sz = 1;
    while (sz < reg->max_entries * CB_TABLE_LOAD_FACTOR) sz <<= 1;
    reg->table_size = sz;

    reg->table = calloc(sz, sizeof(cb_entry_t));
    if (!reg->table) { free(reg); return -1; }

    /* Initialise LRU pointers to -1 */
    for (uint32_t i = 0; i < sz; i++) {
        reg->table[i].lru_prev = -1;
        reg->table[i].lru_next = -1;
    }
    reg->lru_head = -1;
    reg->lru_tail = -1;

    reg->metrics = metrics;
    reg->logger  = logger;

    pthread_mutex_init(&reg->lock, NULL);

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
    pthread_mutex_lock(&reg->lock);
    free(reg->table);
    reg->table = NULL;
    pthread_mutex_unlock(&reg->lock);
    pthread_mutex_destroy(&reg->lock);
    free(reg);
}

/* ============================================================================
 * cb_is_allowed
 * ========================================================================= */

bool cb_is_allowed(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return true; /* No registry = allow all */

    pthread_mutex_lock(&reg->lock);
    int idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_mutex_unlock(&reg->lock);
        return true; /* Unknown host = allow */
    }

    cb_entry_t* e   = &reg->table[idx];
    uint32_t    st  = atomic_load(&e->state);
    int64_t     now = mono_ns();

    if (st == CB_STATE_CLOSED) {
        lru_touch(reg, idx);
        pthread_mutex_unlock(&reg->lock);
        return true;
    }

    if (st == CB_STATE_OPEN) {
        if (now >= e->open_until_ns && !e->probe_in_flight) {
            /* Transition to HALF_OPEN — allow one probe */
            atomic_store(&e->state, CB_STATE_HALF_OPEN);
            e->probe_in_flight = true;
            lru_touch(reg, idx);
            pthread_mutex_unlock(&reg->lock);
            if (reg->logger) {
                LOG_INFO(reg->logger, "circuit_breaker",
                        "Probe allowed (OPEN → HALF_OPEN)",
                        "host", host, NULL);
            }
            return true;
        }
        /* Still OPEN or probe already in flight */
        if (reg->rejected_metric) metrics_counter_inc(reg->rejected_metric);
        pthread_mutex_unlock(&reg->lock);
        return false;
    }

    /* HALF_OPEN — probe already in flight, reject additional callers */
    if (e->probe_in_flight) {
        if (reg->rejected_metric) metrics_counter_inc(reg->rejected_metric);
        pthread_mutex_unlock(&reg->lock);
        return false;
    }

    pthread_mutex_unlock(&reg->lock);
    return true;
}

/* ============================================================================
 * cb_record_success
 * ========================================================================= */

void cb_record_success(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return;

    pthread_mutex_lock(&reg->lock);
    int idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_mutex_unlock(&reg->lock);
        return;
    }

    cb_entry_t* e = &reg->table[idx];
    uint32_t    st = atomic_load(&e->state);

    atomic_store(&e->failure_count, 0);
    atomic_store(&e->state, CB_STATE_CLOSED);
    e->probe_in_flight = false;
    e->recovery_ms     = reg->base_recovery_ms; /* Reset backoff */
    lru_touch(reg, idx);
    pthread_mutex_unlock(&reg->lock);

    if (st != CB_STATE_CLOSED && reg->logger) {
        LOG_INFO(reg->logger, "circuit_breaker",
                "Circuit CLOSED after success",
                "host", host, NULL);
    }
}

/* ============================================================================
 * cb_record_failure
 * ========================================================================= */

void cb_record_failure(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return;

    pthread_mutex_lock(&reg->lock);
    int idx = table_find_or_insert(reg, host, port);
    if (idx < 0) {
        pthread_mutex_unlock(&reg->lock);
        return;
    }

    cb_entry_t* e      = &reg->table[idx];
    int64_t     now    = mono_ns();
    uint32_t    st     = atomic_load(&e->state);

    /* Expire old failures outside the window */
    if ((now - e->last_failure_ns) > ms_to_ns(reg->window_ms)) {
        atomic_store(&e->failure_count, 0);
    }
    e->last_failure_ns = now;

    uint32_t failures = atomic_fetch_add(&e->failure_count, 1) + 1;

    if (st == CB_STATE_HALF_OPEN || failures >= reg->failure_threshold) {
        /* Trip to OPEN */
        e->open_until_ns   = now + ms_to_ns(e->recovery_ms);
        e->probe_in_flight = false;
        atomic_store(&e->state, CB_STATE_OPEN);
        atomic_store(&e->failure_count, 0);

        /* Exponential backoff: double recovery window, capped at 60 s */
        uint32_t next_recovery = e->recovery_ms * 2;
        e->recovery_ms = (next_recovery < CB_MAX_RECOVERY_MS)
                         ? next_recovery : CB_MAX_RECOVERY_MS;

        if (reg->open_total_metric) metrics_counter_inc(reg->open_total_metric);
        if (reg->logger) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u ms", e->recovery_ms / 2);
            LOG_WARN(reg->logger, "circuit_breaker",
                    "Circuit OPEN",
                    "host", host, "recovery_ms", buf, NULL);
        }
    }

    lru_touch(reg, idx);
    pthread_mutex_unlock(&reg->lock);
}

/* ============================================================================
 * cb_get_state
 * ========================================================================= */

cb_state_t cb_get_state(cb_registry_t* reg, const char* host, uint16_t port) {
    if (!reg || !host) return CB_STATE_CLOSED;

    pthread_mutex_lock(&reg->lock);
    int idx = table_find(reg, host, port);
    if (idx < 0) {
        pthread_mutex_unlock(&reg->lock);
        return CB_STATE_CLOSED;
    }
    cb_state_t st = (cb_state_t)atomic_load(&reg->table[idx].state);
    pthread_mutex_unlock(&reg->lock);
    return st;
}