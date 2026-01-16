/**
 * @file http_server.h
 * @brief Internal HTTP server implementation details
 * 
 * This header contains implementation-specific structures.
 * Public API is in distric_obs.h
 */

#ifndef DISTRIC_OBS_HTTP_SERVER_H
#define DISTRIC_OBS_HTTP_SERVER_H

#include "distric_obs.h"
#include <pthread.h>
#include <stdatomic.h>

/* HTTP request structure - internal */
typedef struct {
    char method[16];
    char path[256];
    char version[16];
} http_request_t;

/* HTTP response structure - internal */
typedef struct {
    int status_code;
    const char* status_text;
    const char* content_type;
    const char* body;
    size_t body_length;
} http_response_t;

/* Observability HTTP server - internal implementation */
struct obs_server_s {
    int socket_fd;
    uint16_t port;
    pthread_t thread;
    _Atomic bool running;
    
    /* References to observability components */
    metrics_registry_t* metrics;
    health_registry_t* health;
};

#endif /* DISTRIC_OBS_HTTP_SERVER_H */