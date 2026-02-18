/**
 * @file http_server_internal.h
 * @brief DistriC Observability — HTTP Server Internal Details
 *
 * NOT part of the public API.  Only http_server.c may include this header.
 *
 * Security model:
 *   - Read timeout enforced via SO_RCVTIMEO on accepted socket.
 *   - Write timeout enforced via SO_SNDTIMEO on accepted socket.
 *   - Max request line length: HTTP_MAX_REQUEST_BYTES.
 *   - Response data is ALWAYS copied from registry before connection handling.
 *     The server holds no registry locks while writing to the network.
 *   - Only GET requests accepted; others receive 405.
 *   - Path traversal patterns rejected with 400.
 *   - Only recognised paths return data; all others receive 404.
 */

#ifndef DISTRIC_HTTP_SERVER_INTERNAL_H
#define DISTRIC_HTTP_SERVER_INTERNAL_H

#include "distric_obs.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>

/* ============================================================================
 * Internal defaults
 * ========================================================================= */

#define HTTP_DEFAULT_READ_TIMEOUT_MS    5000u
#define HTTP_DEFAULT_WRITE_TIMEOUT_MS   10000u
#define HTTP_DEFAULT_MAX_RESPONSE_BYTES (4u * 1024u * 1024u)   /* 4 MiB */
#define HTTP_MAX_REQUEST_BYTES          4096u
#define HTTP_MAX_PATH_LEN               256u
#define HTTP_MAX_METHOD_LEN             8u
#define HTTP_ACCEPT_BACKLOG             32

/* ============================================================================
 * Internal types
 * ========================================================================= */

typedef struct {
    char method[HTTP_MAX_METHOD_LEN];
    char path[HTTP_MAX_PATH_LEN];
} http_request_t;

typedef struct {
    int         status_code;
    const char* status_text;
    const char* content_type;
    char*       body;            /* heap-allocated; freed after send */
    size_t      body_length;
} http_response_t;

struct obs_server_s {
    int                 listen_fd;
    uint16_t            port;
    metrics_registry_t* metrics;
    health_registry_t*  health;

    uint32_t            read_timeout_ms;
    uint32_t            write_timeout_ms;
    size_t              max_response_bytes;

    pthread_t           accept_thread;
    bool                thread_started;
    _Atomic bool        shutdown;
};

#endif /* DISTRIC_HTTP_SERVER_INTERNAL_H */