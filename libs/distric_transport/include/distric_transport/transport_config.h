/**
 * @file transport_config.h
 * @brief DistriC Transport — Global Runtime Configuration v1
 *
 * Provides a centralised, process-wide configuration object for all transport
 * subsystems (TCP, UDP, connection pool). Configuration is loaded once at
 * process start and applied atomically. Key limits may be updated at runtime
 * via the API without restarting the process.
 *
 * ==========================================================================
 * MEMORY BUDGET (Item 2)
 * ==========================================================================
 *
 * A global atomic counter tracks all bytes allocated for send-queue ring
 * buffers across all active connections. When the total exceeds
 * max_transport_memory_bytes, new connection allocation is rejected and the
 * caller receives DISTRIC_ERR_ALLOC_FAILURE. This prevents unbounded heap
 * growth under sustained downstream slowdowns.
 *
 * Counter manipulation:
 *   transport_memory_acquire(bytes) — called by connection_alloc(); returns
 *                                      false if the limit would be exceeded.
 *   transport_memory_release(bytes) — called by tcp_close() / udp_close().
 *
 * ==========================================================================
 * OBSERVABILITY SAMPLING (Item 3)
 * ==========================================================================
 *
 * Under extreme load, verbose logging amplifies latency. Setting
 * observability_sample_pct < 100 causes the transport to skip non-critical
 * log calls with probability (100 - pct) / 100. This uses a thread-local
 * LCG — no locks, no allocations.
 *
 * ==========================================================================
 * ENVIRONMENT VARIABLE OVERRIDES (Item 6)
 * ==========================================================================
 *
 * transport_config_load_env() populates a config from environment variables:
 *
 *   DISTRIC_MAX_TRANSPORT_MEMORY_BYTES   size_t
 *   DISTRIC_OBS_SAMPLE_PCT               uint32_t  (1–100)
 *   DISTRIC_HANDLER_WARN_DURATION_MS     uint32_t
 *
 * @version 1.0.0
 */

#ifndef DISTRIC_TRANSPORT_CONFIG_H
#define DISTRIC_TRANSPORT_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * GLOBAL TRANSPORT CONFIGURATION STRUCT
 * ========================================================================= */

/**
 * @brief Process-wide transport configuration.
 *
 * Zero-initialise to use all defaults (safe, unlimited).
 * Pass to transport_config_apply() before creating any server or socket.
 */
typedef struct {
    /**
     * Maximum bytes held in all transport send-queue ring buffers combined.
     * 0 = unlimited (default — matches legacy behaviour).
     * Recommended: 256 MB for services handling up to 100k connections.
     */
    size_t max_transport_memory_bytes;

    /**
     * Percentage of non-critical transport log calls to emit.
     * Range: 1–100.  0 = use default (100%).
     * Applies to LOG_INFO and LOG_DEBUG calls in the accept and I/O paths.
     * LOG_WARN and LOG_ERROR are never sampled — they are always emitted.
     */
    uint32_t observability_sample_pct;

    /**
     * Log a warning when a connection handler runs longer than this many ms.
     * 0 = disabled (default).
     * Applied globally; overridden per-server via tcp_server_config_t.
     */
    uint32_t handler_warn_duration_ms;
} transport_global_config_t;

/* ============================================================================
 * LIFECYCLE
 * ========================================================================= */

/**
 * @brief Initialise @p cfg with safe defaults.
 *
 * Fills all fields with production-safe defaults:
 *   max_transport_memory_bytes = 0   (unlimited)
 *   observability_sample_pct   = 100
 *   handler_warn_duration_ms   = 0   (disabled)
 */
void transport_config_init(transport_global_config_t *cfg);

/**
 * @brief Apply @p cfg as the global effective configuration.
 *
 * Atomic: in-flight connections are not affected mid-operation.
 * Safe to call at any time; limits take effect for all subsequent
 * connection_alloc() calls.
 *
 * @return DISTRIC_OK always (provided cfg is non-NULL).
 */
distric_err_t transport_config_apply(const transport_global_config_t *cfg);

/**
 * @brief Populate @p cfg from environment variables.
 *
 * Reads and parses DISTRIC_MAX_TRANSPORT_MEMORY_BYTES,
 * DISTRIC_OBS_SAMPLE_PCT, and DISTRIC_HANDLER_WARN_DURATION_MS.
 * Fields not set in the environment retain their current value in @p cfg.
 * Call transport_config_init() first to ensure a clean baseline.
 *
 * @param cfg  [in,out] Configuration to populate.
 */
void transport_config_load_env(transport_global_config_t *cfg);

/**
 * @brief Copy the current effective global configuration into @p out.
 */
void transport_config_get(transport_global_config_t *out);

/* ============================================================================
 * METRICS REGISTRATION
 * ========================================================================= */

/**
 * @brief Register global transport memory and obs metrics.
 *
 * Call once after creating a metrics_registry_t. Registers:
 *   transport_memory_bytes_in_use   gauge  (current send-queue bytes)
 *   connections_rejected_memory_total counter (rejections due to memory limit)
 *   observability_dropped_events_total counter (sampled-out obs events)
 *
 * All metrics are updated via the same atomic counters used by the hot paths
 * — no locking on the update side.
 *
 * @param registry  Metrics registry to register into (must outlive transport).
 * @return DISTRIC_OK on success.
 */
distric_err_t transport_config_register_metrics(metrics_registry_t *registry);

/* ============================================================================
 * INTERNAL — used by tcp.c and udp.c ONLY
 * ========================================================================= */

/**
 * @brief Attempt to reserve @p bytes of transport memory.
 *
 * Atomically increments the global memory counter. Returns false without
 * modifying the counter if adding @p bytes would exceed the configured
 * limit. Returns true unconditionally if limit is 0 (unlimited).
 *
 * Thread-safe, lock-free.
 */
bool transport_memory_acquire(size_t bytes);

/**
 * @brief Release @p bytes previously acquired via transport_memory_acquire().
 *
 * Must be called with the same byte count passed to acquire.
 * Thread-safe, lock-free.
 */
void transport_memory_release(size_t bytes);

/**
 * @brief Return the current number of bytes in use across all send queues.
 */
size_t transport_memory_in_use(void);

/**
 * @brief Return true if non-critical observability should be emitted now.
 *
 * Uses a thread-local LCG — no locks, branch-predictor-friendly.
 * WARN and ERROR calls must bypass this check (always log).
 */
bool transport_should_sample_obs(void);

/**
 * @brief Increment the dropped observability events counter.
 *
 * Called when transport_should_sample_obs() returns false and a log/metric
 * call is skipped to provide visibility into sampling impact.
 */
void transport_obs_record_drop(void);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_CONFIG_H */