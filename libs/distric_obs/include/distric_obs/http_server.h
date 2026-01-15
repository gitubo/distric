#ifndef DISTRIC_OBS_HTTP_SERVER_H
#define DISTRIC_OBS_HTTP_SERVER_H

#include "distric_obs.h"
#include "distric_obs/metrics.h"
#include "distric_obs/health.h"
#include <pthread.h>
#include <stdatomic.h>

/* HTTP request structure */
typedef struct {
    char method[16];
    char path[256];
    char version[16];
} http_request_t;

/* HTTP response structure */
typedef struct {
    int status_code;
    const char* status_text;
    const char* content_type;
    const char* body;
    size_t body_length;
} http_response_t;

/* Observability HTTP server */
typedef struct obs_server_s {
    int socket_fd;
    uint16_t port;
    pthread_t thread;
    _Atomic bool running;
    
    /* References to observability components */
    metrics_registry_t* metrics;
    health_registry_t* health;
} obs_server_t;

/* Initialize and start HTTP server */
distric_err_t obs_server_init(obs_server_t** server,
                              uint16_t port,
                              metrics_registry_t* metrics,
                              health_registry_t* health);

/* Stop and destroy HTTP server */
void obs_server_destroy(obs_server_t* server);

/* Get server port (useful if port 0 was used for auto-assignment) */
uint16_t obs_server_get_port(obs_server_t* server);

#endif /* DISTRIC_OBS_HTTP_SERVER_H */