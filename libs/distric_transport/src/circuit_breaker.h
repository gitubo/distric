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
 *  CLOSED    Normal operation. Failures counted within window_ms. When
 *            failure count reaches threshold, transition → OPEN.
 *
 *  OPEN      All connection attempts rejected immediately. After recovery_ms
 *            milliseconds, one probe is allowed → HALF_OPEN.
 *
 *  HALF_OPEN One probe attempt in flight. On success → CLOSED (counters
 *            reset). On failure → OPEN (timer restarted with exponential
 *            backoff, capped at 60 s).
 *
 * ==========================================================================
 * THREAD SAFETY — v2
 * ==========================================================================
 *
 *  Registry locking uses pthread_rwlock_t instead of a plain mutex.
 *
 *  Read lock (shared, concurrent):
 *    cb_is_allowed() CLOSED fast-path, cb_get_state().
 *    Multiple callers proceed concurrently. No writer is blocked for the
 *    dominant case (steady-state CLOSED host).
 *
 *  Write lock (exclusive):
 *    cb_record_failure(), cb_record_success(), insertion/eviction, and the
 *    OPEN → HALF_OPEN transition inside cb_is_allowed(). These are rare
 *    relative to the read path.
 *
 *  Per-entry atomics (state, failure_count) provide fine-grained visibility
 *  without additional locking for the read fast-path.
 *
 *  Double-check pattern in cb_is_allowed():
 *    1. Take rdlock → find entry → read state.
 *    2. If CLOSED → return under rdlock (no write needed).
 *    3. Release rdlock → take wrlock → re-find → handle transition.
 *
 * ==========================================================================
 * DEFAULTS
 * ==========================================================================
 *
 *  failure_threshold  5  failures
 *  recovery_ms        5000 ms  (doubles on each trip, capped at 60 s)
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
 * OPAQUE TYPE
 * ========================================================================= */

typedef struct cb_registry_s cb_registry_t;

/* ============================================================================
 * CIRCUIT BREAKER STATE
 * ========================================================================= */

typedef enum {
    CB_STATE_CLOSED    = 0,  /**< Normal — all connections allowed.           */
    CB_STATE_OPEN      = 1,  /**< Tripped — all connections rejected.         */
    CB_STATE_HALF_OPEN = 2,  /**< One probe allowed; outcome resets or retips.*/
} cb_state_t;

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
 * @param cfg      Configuration (NULL = defaults).
 * @param metrics  Metrics registry (may be NULL).
 * @param logger   Logger (may be NULL).
 * @param out      [out] Created registry handle.
 * @return 0 on success, -1 on allocation failure.
 */
int cb_registry_create(
    const cb_config_t*  cfg,
    metrics_registry_t* metrics,
    logger_t*           logger,
    cb_registry_t**     out
);

/**
 * @brief Destroy the registry and free all resources.
 */
void cb_registry_destroy(cb_registry_t* reg);

/* ============================================================================
 * CIRCUIT BREAKER API
 * ========================================================================= */

/**
 * @brief Check if a connection attempt to host:port is allowed.
 *
 * CLOSED fast path uses a shared read lock — multiple callers concurrent.
 * OPEN/HALF_OPEN transition uses exclusive write lock — rare path.
 *
 * @param reg   Registry handle.
 * @param host  Target hostname or IP.
 * @param port  Target port.
 * @return true if the connection should proceed; false if the circuit is open.
 */
bool cb_is_allowed(cb_registry_t* reg, const char* host, uint16_t port);

/**
 * @brief Record a successful connection to host:port.
 *
 * If the circuit was OPEN or HALF_OPEN, transitions to CLOSED and resets
 * the failure counter and backoff.
 */
void cb_record_success(cb_registry_t* reg, const char* host, uint16_t port);

/**
 * @brief Record a failed connection attempt to host:port.
 *
 * Increments the failure counter. Transitions to OPEN when the failure
 * threshold is exceeded within the counting window. Applies exponential
 * backoff to the recovery timer.
 */
void cb_record_failure(cb_registry_t* reg, const char* host, uint16_t port);

/**
 * @brief Return the current circuit state for host:port.
 *
 * Uses a shared read lock. Safe to call from any number of threads.
 */
cb_state_t cb_get_state(cb_registry_t* reg, const char* host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CIRCUIT_BREAKER_H */