#ifndef DISTRIC_OBS_HEALTH_H
#define DISTRIC_OBS_HEALTH_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>

/* Health status */
typedef enum {
    HEALTH_UP = 0,
    HEALTH_DEGRADED = 1,
    HEALTH_DOWN = 2,
} health_status_t;

/* Maximum limits */
#define MAX_HEALTH_COMPONENTS 64
#define MAX_COMPONENT_NAME_LEN 64
#define MAX_HEALTH_MESSAGE_LEN 256

/* Health check component */
typedef struct {
    char name[MAX_COMPONENT_NAME_LEN];
    _Atomic int status;  /* health_status_t */
    char message[MAX_HEALTH_MESSAGE_LEN];
    uint64_t last_check_time_ms;
    _Atomic bool active;
} health_component_t;

/* Health registry */
typedef struct health_registry_s {
    health_component_t components[MAX_HEALTH_COMPONENTS];
    _Atomic size_t component_count;
    pthread_mutex_t lock;
} health_registry_t;

/* Initialize health registry */
distric_err_t health_init(health_registry_t** registry);

/* Destroy health registry */
void health_destroy(health_registry_t* registry);

/* Register a health check component */
distric_err_t health_register_component(health_registry_t* registry,
                                        const char* name,
                                        health_component_t** out_component);

/* Update component health status */
distric_err_t health_update_status(health_component_t* component,
                                   health_status_t status,
                                   const char* message);

/* Get overall system health (worst status of all components) */
health_status_t health_get_overall_status(health_registry_t* registry);

/* Export health status as JSON */
distric_err_t health_export_json(health_registry_t* registry,
                                 char** out_buffer,
                                 size_t* out_size);

/* Helper: Convert health status to string */
const char* health_status_str(health_status_t status);

#endif /* DISTRIC_OBS_HEALTH_H */