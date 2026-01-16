/**
 * @file health.h
 * @brief Internal health monitoring implementation details
 * 
 * This header contains implementation-specific structures.
 * Public API is in distric_obs.h
 */

#ifndef DISTRIC_OBS_HEALTH_H
#define DISTRIC_OBS_HEALTH_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>

/* Maximum limits */
#define MAX_HEALTH_COMPONENTS 64
#define MAX_COMPONENT_NAME_LEN 64
#define MAX_HEALTH_MESSAGE_LEN 256

/* Health check component - internal implementation */
struct health_component_s {
    char name[MAX_COMPONENT_NAME_LEN];
    _Atomic int status;  /* health_status_t */
    char message[MAX_HEALTH_MESSAGE_LEN];
    uint64_t last_check_time_ms;
    _Atomic bool active;
};

/* Health registry - internal implementation */
struct health_registry_s {
    health_component_t components[MAX_HEALTH_COMPONENTS];
    _Atomic size_t component_count;
    pthread_mutex_t lock;
};

#endif /* DISTRIC_OBS_HEALTH_H */