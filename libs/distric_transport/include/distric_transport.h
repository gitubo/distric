/**
 * @file distric_transport.h
 * @brief DistriC Transport Layer — Single Public Header v3
 *
 * This is the ONLY header that consumers of distric_transport need to include.
 *
 * Modules:
 *   - Transport global configuration (memory budget, obs sampling, env overrides)
 *   - TCP server and client (non-blocking, epoll-based, worker pool)
 *   - TCP connection pool (LRU eviction, O(1) hash lookup, thread-safe)
 *   - UDP socket (non-blocking, per-peer rate limiting)
 *   - Transport error taxonomy (stable classification of OS errors)
 *
 * ==========================================================================
 * WHAT'S NEW IN v3
 * ==========================================================================
 *
 * 1. WORKER POOL SATURATION PROTECTION
 *    - tcp_server_config_t.handler_warn_duration_ms: log slow handlers.
 *    - tcp_server_worker_busy_ratio(): query real-time worker utilisation.
 *    - Metrics: tcp_worker_busy_ratio, tcp_handler_slow_total,
 *               tcp_handler_duration_max_ms, tcp_worker_queue_depth.
 *
 * 2. GLOBAL TRANSPORT MEMORY BUDGET
 *    - transport_global_config_t.max_transport_memory_bytes: process-wide
 *      ceiling for all send-queue allocations. Connections are rejected
 *      (not processes killed) when the ceiling is reached.
 *    - Metrics: transport_memory_bytes_in_use,
 *               connections_rejected_memory_total.
 *
 * 3. OBSERVABILITY ISOLATION
 *    - transport_global_config_t.observability_sample_pct: drop non-critical
 *      log calls under load. WARN and ERROR are never sampled.
 *    - Metric: observability_dropped_events_total.
 *
 * 4. CONFIGURATION EXTERNALIZATION
 *    - transport_config_init() / transport_config_apply() / transport_config_load_env()
 *    - Env vars: DISTRIC_MAX_TRANSPORT_MEMORY_BYTES, DISTRIC_OBS_SAMPLE_PCT,
 *                DISTRIC_HANDLER_WARN_DURATION_MS.
 *    - Optional: transport_config_register_metrics() for observability of the
 *      config module itself.
 *
 * 5. API STABILITY MARKERS
 *    - DISTRIC_DEPRECATED(msg) attribute on items scheduled for removal.
 *    - Semantic versioning: major incremented on any breaking change.
 *    - See individual headers for migration guides.
 *
 * ==========================================================================
 * QUICK START (v3)
 * ==========================================================================
 *
 * @code
 * #include <distric_transport.h>
 *
 * // 0. Configure global transport constraints (call once at startup)
 * transport_global_config_t tcfg;
 * transport_config_init(&tcfg);
 * transport_config_load_env(&tcfg);          // honour env overrides
 * tcfg.max_transport_memory_bytes = 256 * 1024 * 1024;  // 256 MB ceiling
 * tcfg.handler_warn_duration_ms   = 1000;   // warn on handlers > 1s
 * transport_config_apply(&tcfg);
 *
 * // 1. Initialize observability (distric_obs)
 * metrics_registry_t* metrics;
 * logger_t*           logger;
 * metrics_init(&metrics);
 * log_init(&logger, STDOUT_FILENO, LOG_MODE_ASYNC);
 *
 * // 2. Register global transport metrics (optional but recommended)
 * transport_config_register_metrics(metrics);
 *
 * // 3. TCP SERVER
 * tcp_server_config_t scfg = {
 *     .worker_threads          = 8,
 *     .handler_warn_duration_ms = 500,
 *     .observability_sample_pct = 10,   // 10% accept-log sampling under load
 * };
 * tcp_server_t* server;
 * tcp_server_create_with_config("0.0.0.0", 9000, &scfg, metrics, logger, &server);
 * tcp_server_start(server, on_connection_callback, NULL);
 *
 * // 4. Query worker health at runtime
 * float busy = tcp_server_worker_busy_ratio(server);
 *
 * // 5. TCP CLIENT with backpressure
 * tcp_connection_t* conn;
 * tcp_connect("10.0.1.5", 9000, 5000, NULL, metrics, logger, &conn);
 * int rc = tcp_send(conn, data, len);
 * if (rc == DISTRIC_ERR_BACKPRESSURE) { ... }
 *
 * // 6. TCP POOL
 * tcp_pool_t* pool;
 * tcp_pool_create(100, metrics, logger, &pool);
 * tcp_pool_acquire(pool, "10.0.1.5", 9000, &conn);
 * tcp_send(conn, data, len);
 * tcp_pool_release(pool, conn);
 *
 * // 7. UDP with rate limiting
 * udp_rate_limit_config_t rl = { .rate_limit_pps = 1000, .burst_size = 2000 };
 * udp_socket_t* udp;
 * udp_socket_create("0.0.0.0", 9001, &rl, metrics, logger, &udp);
 * udp_send(udp, data, len, "10.0.1.6", 9001);
 *
 * // 8. Cleanup (reverse order)
 * udp_close(udp);
 * tcp_pool_destroy(pool);
 * tcp_close(conn);
 * tcp_server_destroy(server);
 * log_destroy(logger);
 * metrics_destroy(metrics);
 * @endcode
 *
 * @version 3.0.0
 */

#ifndef DISTRIC_TRANSPORT_H
#define DISTRIC_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * GLOBAL CONFIGURATION (include first — establishes memory/obs contracts)
 * ========================================================================= */

/**
 * @defgroup transport_config Transport Global Configuration
 * @brief Memory budget, observability sampling, environment variable overrides.
 * @{
 */
#include "distric_transport/transport_config.h"
/** @} */

/* ============================================================================
 * ERROR TAXONOMY (include before transport modules)
 * ========================================================================= */

/**
 * @defgroup transport_error Transport Error Taxonomy
 * @brief Stable classification of OS errors into transport categories.
 * @{
 */
#include "distric_transport/transport_error.h"
/** @} */

/* ============================================================================
 * TCP TRANSPORT
 * ========================================================================= */

/**
 * @defgroup tcp TCP Transport
 * @brief Non-blocking, epoll-based TCP server and client with backpressure.
 *
 * Key guarantees (v5):
 *  - All sockets are O_NONBLOCK; no library call blocks indefinitely.
 *  - tcp_send() returns DISTRIC_ERR_BACKPRESSURE when the per-connection
 *    send queue exceeds its high-water mark.
 *  - Worker pool saturation is tracked and exported (tcp_worker_busy_ratio).
 *  - Slow handlers trigger LOG_WARN and increment tcp_handler_slow_total.
 *  - New connections are rejected before OOM via the global memory budget.
 * @{
 */
#include "distric_transport/tcp.h"
/** @} */

/* ============================================================================
 * TCP CONNECTION POOL
 * ========================================================================= */

/**
 * @defgroup tcp_pool TCP Connection Pool
 * @brief Thread-safe connection reuse with O(1) hash lookup and LRU eviction.
 * @{
 */
#include "distric_transport/tcp_pool.h"
/** @} */

/* ============================================================================
 * UDP TRANSPORT
 * ========================================================================= */

/**
 * @defgroup udp UDP Transport
 * @brief Non-blocking UDP with per-peer token-bucket rate limiting.
 * @{
 */
#include "distric_transport/udp.h"
/** @} */

/* ============================================================================
 * VERSION
 * ========================================================================= */

/** Library version — follows semantic versioning. */
#define DISTRIC_TRANSPORT_VERSION_MAJOR 3
#define DISTRIC_TRANSPORT_VERSION_MINOR 0
#define DISTRIC_TRANSPORT_VERSION_PATCH 0
#define DISTRIC_TRANSPORT_VERSION_STR   "3.0.0"

/**
 * @brief Minimum API version this library is backward-compatible with.
 *
 * Consumers compiled against DISTRIC_TRANSPORT_COMPAT_MAJOR will continue
 * to function without source changes unless they use items marked
 * DISTRIC_DEPRECATED.
 */
#define DISTRIC_TRANSPORT_COMPAT_MAJOR  2

static inline const char* distric_transport_version(void) {
    return DISTRIC_TRANSPORT_VERSION_STR;
}

/**
 * @brief Compile-time version check.
 *
 * Example: Abort compilation if library is older than 3.0.
 * @code
 * #if !DISTRIC_TRANSPORT_VERSION_AT_LEAST(3, 0, 0)
 * #  error "distric_transport >= 3.0.0 required"
 * #endif
 * @endcode
 */
#define DISTRIC_TRANSPORT_VERSION_AT_LEAST(maj, min, pat) \
    ((DISTRIC_TRANSPORT_VERSION_MAJOR > (maj)) || \
     (DISTRIC_TRANSPORT_VERSION_MAJOR == (maj) && \
      DISTRIC_TRANSPORT_VERSION_MINOR > (min)) || \
     (DISTRIC_TRANSPORT_VERSION_MAJOR == (maj) && \
      DISTRIC_TRANSPORT_VERSION_MINOR == (min) && \
      DISTRIC_TRANSPORT_VERSION_PATCH >= (pat)))

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_H */