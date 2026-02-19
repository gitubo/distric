/**
 * @file circuit_breaker.h
 * @brief Per-host circuit breaker for TCP connection failure containment.
 *
 * INTERNAL — do not include from outside distric_transport sources.
 *
 * ==========================================================================
 * STATES
 * ==========================================================================
 *
 *  CLOSED    Normal operation. Failures are counted. When failure count
 *            reaches the threshold within the window, transition → OPEN.
 *
 *  OPEN      All connection attempts are rejected immediately with
 *            DISTRIC_ERR_CIRCUIT_OPEN. After recovery_ms milliseconds,
 *            one probe is allowed → HALF_OPEN.
 *
 *  HALF_OPEN One probe attempt in flight. On success → CLOSED (counters
 *            reset). On failure → OPEN (timer restarted with backoff).
 *
 * ==========================================================================
 * THREAD SAFETY
 * ==========================================================================
 *
 *  All state transitions are protected by a per-entry spinlock (atomic
 *  compare-exchange). The global registry uses a single mutex for insertion
 *  and lookup, but the hot path (state check) uses only the per-entry lock.
 *
 * ==========================================================================
 * DEFAULTS
 * ==========================================================================
 *
 *  failure_threshold  5  failures
 *  recovery_ms        5000 ms  (doubles on each consecutive OPEN event, up to 60 s)
 *  window_ms          10000 ms (sliding window for failure counting)
 *  max_entries        1024     (LRU eviction when full)
 */

#ifndef DISTRIC_CIRCUIT_BREAKER_H
#define DISTRIC_CIRCUIT_BREAKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <distric_obs.h>
#include "distric_transport/transport_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * OPAQUE TYPES
 * ========================================================================= */

typedef struct cb_registry_s cb_registry_t;

/* ============================================================================
 * CONFIGURATION
 * ========================================================================= */

typedef struct {
    uint32_t failure_threshold;  /**< Failures before OPEN. 0 = 5.        */
    uint32_t recovery_ms;        /**< Base open duration.  0 = 5000 ms.   */
    uint32_t window_ms;          /**< Failure count window. 0 = 10000 ms. */
    uint32_t max_entries;        /**< Max hosts tracked.   0 = 1024.      */
} cb_config_t;

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

/**
 * @brief Allocate a circuit-breaker registry.
 *
 * One registry per server or pool instance is sufficient.
 *
 * @param cfg      Configuration (NULL = defaults).
 * @param metrics  Metrics registry for breaker state exports (may be NULL).
 * @param logger   Logger (may be NULL).
 * @param out      [out] Created registry.
 * @return 0 on success, -1 on allocation failure.
 */
int cb_registry_create(
    const cb_config_t*  cfg,
    metrics_registry_t* metrics,
    logger_t*           logger,
    cb_registry_t**     out
);

/**
 * @brief Destroy a registry and free all memory.
 */
void cb_registry_destroy(cb_registry_t* reg);

/* ============================================================================
 * OPERATIONS
 * ========================================================================= */

/**
 * @brief Check whether a connection to host:port is permitted.
 *
 * Called BEFORE attempting tcp_connect.
 *
 * @return true   Circuit is CLOSED or in HALF_OPEN probe window → proceed.
 * @return false  Circuit is OPEN → reject immediately.
 */
bool cb_is_allowed(cb_registry_t* reg, const char* host, uint16_t port);

/**
 * @brief Record a successful connection or operation to host:port.
 *
 * Resets the failure counter. Transitions HALF_OPEN → CLOSED.
 */
void cb_record_success(cb_registry_t* reg, const char* host, uint16_t port);

/**
 * @brief Record a failed connection attempt to host:port.
 *
 * Increments the failure counter. May transition CLOSED → OPEN or
 * HALF_OPEN → OPEN.
 */
void cb_record_failure(cb_registry_t* reg, const char* host, uint16_t port);

/* ============================================================================
 * INTROSPECTION
 * ========================================================================= */

typedef enum {
    CB_STATE_CLOSED    = 0,
    CB_STATE_OPEN      = 1,
    CB_STATE_HALF_OPEN = 2,
} cb_state_t;

/**
 * @brief Return the current breaker state for host:port.
 *
 * Returns CB_STATE_CLOSED if the host is not tracked.
 */
cb_state_t cb_get_state(cb_registry_t* reg, const char* host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CIRCUIT_BREAKER_H */