/**
 * @file transport_error.c
 * @brief Transport error taxonomy — classification and conversion.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "distric_transport/transport_error.h"
#include <errno.h>
#include <string.h>

/* ============================================================================
 * ERRNO CLASSIFICATION
 * ========================================================================= */

transport_err_t transport_classify_errno(int err_no) {
    switch (err_no) {
        /* Non-blocking: retry path */
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return TRANSPORT_WOULD_BLOCK;

        /* Peer-initiated graceful close */
        case 0:  /* recv returned 0 → EOF */
            return TRANSPORT_PEER_CLOSED;

        /* Abrupt connection termination */
        case ECONNRESET:
        case EPIPE:
        case ECONNABORTED:
            return TRANSPORT_RESET;

        /* Connection/operation timeout */
        case ETIMEDOUT:
            return TRANSPORT_TIMEOUT;

        /* Address & routing issues */
        case EADDRNOTAVAIL:
        case EADDRINUSE:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ECONNREFUSED:
        case ENONET:
            return TRANSPORT_ADDRESS;

        /* OS resource exhaustion */
        case ENOMEM:
        case ENFILE:
        case EMFILE:
        case ENOBUFS:
        case ENOSPC:
            return TRANSPORT_RESOURCE;

        /* Unexpected / internal */
        default:
            return TRANSPORT_INTERNAL;
    }
}

/* ============================================================================
 * STRING REPRESENTATION
 * ========================================================================= */

const char* transport_err_str(transport_err_t err) {
    switch (err) {
        case TRANSPORT_OK:           return "OK";
        case TRANSPORT_WOULD_BLOCK:  return "WOULD_BLOCK";
        case TRANSPORT_BACKPRESSURE: return "BACKPRESSURE";
        case TRANSPORT_PEER_CLOSED:  return "PEER_CLOSED";
        case TRANSPORT_RESET:        return "RESET";
        case TRANSPORT_TIMEOUT:      return "TIMEOUT";
        case TRANSPORT_ADDRESS:      return "ADDRESS_ERROR";
        case TRANSPORT_RESOURCE:     return "RESOURCE_EXHAUSTED";
        case TRANSPORT_RATE_LIMITED: return "RATE_LIMITED";
        case TRANSPORT_INTERNAL:     return "INTERNAL_ERROR";
        default:                     return "UNKNOWN";
    }
}

/* ============================================================================
 * CONVERSION TO distric_err_t
 * ========================================================================= */

distric_err_t transport_err_to_distric(transport_err_t err) {
    switch (err) {
        case TRANSPORT_OK:           return DISTRIC_OK;
        case TRANSPORT_WOULD_BLOCK:  return DISTRIC_OK;          /* non-fatal */
        case TRANSPORT_BACKPRESSURE: return DISTRIC_ERR_BACKPRESSURE;
        case TRANSPORT_PEER_CLOSED:  return DISTRIC_ERR_EOF;
        case TRANSPORT_RESET:        return DISTRIC_ERR_IO;
        case TRANSPORT_TIMEOUT:      return DISTRIC_ERR_TIMEOUT;
        case TRANSPORT_ADDRESS:      return DISTRIC_ERR_INIT_FAILED;
        case TRANSPORT_RESOURCE:     return DISTRIC_ERR_ALLOC_FAILURE;
        case TRANSPORT_RATE_LIMITED: return DISTRIC_ERR_BACKPRESSURE;
        case TRANSPORT_INTERNAL:
        default:                     return DISTRIC_ERR_IO;
    }
}