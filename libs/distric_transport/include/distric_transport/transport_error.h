/**
 * @file transport_error.h
 * @brief DistriC Transport — Stable Error Taxonomy
 *
 * Maps OS-level errno values to a stable, transport-agnostic error
 * classification. All transport APIs return these codes so operators
 * can reason about failures without OS-specific knowledge.
 *
 * Design principles:
 *  - Each code maps to a single actionable category.
 *  - Codes are stable across OS versions and protocol changes.
 *  - Every code has a defined operator response (retry, backoff, alert).
 *
 * @version 2.0.0
 */

#ifndef DISTRIC_TRANSPORT_ERROR_H
#define DISTRIC_TRANSPORT_ERROR_H

#include <distric_obs.h>  /* distric_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TRANSPORT ERROR ENUM
 * ========================================================================= */

/**
 * @brief Stable transport error codes.
 *
 * Operator response guide:
 *  TRANSPORT_OK            — No action needed.
 *  TRANSPORT_WOULD_BLOCK   — Retry immediately or register for write-readiness.
 *  TRANSPORT_BACKPRESSURE  — Apply upstream flow control; slow down producers.
 *  TRANSPORT_PEER_CLOSED   — Graceful peer shutdown; clean up the connection.
 *  TRANSPORT_RESET         — Abrupt peer reset; log and reconnect.
 *  TRANSPORT_TIMEOUT       — Connection or I/O timeout; reconnect with backoff.
 *  TRANSPORT_ADDRESS       — Bad host/port; fix configuration.
 *  TRANSPORT_RESOURCE      — OS resource exhaustion; reduce load or tune limits.
 *  TRANSPORT_RATE_LIMITED  — UDP peer exceeded rate limit; drop is expected.
 *  TRANSPORT_INTERNAL      — Unexpected OS error; log errno and alert.
 */
typedef enum {
    TRANSPORT_OK            =  0,  /**< Success, no error.                          */
    TRANSPORT_WOULD_BLOCK   =  1,  /**< I/O would block; no data ready (non-fatal). */
    TRANSPORT_BACKPRESSURE  =  2,  /**< Send queue above HWM; slow down producers.  */
    TRANSPORT_PEER_CLOSED   =  3,  /**< EOF received; peer closed gracefully.        */
    TRANSPORT_RESET         =  4,  /**< Connection reset (ECONNRESET / EPIPE).       */
    TRANSPORT_TIMEOUT       =  5,  /**< Operation timed out.                         */
    TRANSPORT_ADDRESS       =  6,  /**< Address resolution or bind failure.          */
    TRANSPORT_RESOURCE      =  7,  /**< Resource exhaustion (ENOMEM, EMFILE, etc.).  */
    TRANSPORT_RATE_LIMITED  =  8,  /**< UDP packet dropped: rate limit exceeded.     */
    TRANSPORT_INTERNAL      =  9,  /**< Unexpected OS-level error.                   */
} transport_err_t;

/* ============================================================================
 * CLASSIFICATION & CONVERSION
 * ========================================================================= */

/**
 * @brief Map an errno value to a stable transport error code.
 *
 * @param err_no  errno value (from <errno.h>).
 * @return        Corresponding transport_err_t category.
 */
transport_err_t transport_classify_errno(int err_no);

/**
 * @brief Return a human-readable string for a transport error code.
 *
 * @param err  Transport error code.
 * @return     Static string (never NULL).
 */
const char* transport_err_str(transport_err_t err);

/**
 * @brief Convert transport_err_t to distric_err_t.
 *
 * Used by public APIs that return distric_err_t.
 *
 * @param err  Transport error code.
 * @return     Equivalent distric_err_t value.
 */
distric_err_t transport_err_to_distric(transport_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_TRANSPORT_ERROR_H */