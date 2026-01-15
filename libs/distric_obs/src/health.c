#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "distric_obs/health.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

/* Get current timestamp in milliseconds */
static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* Convert health status to string */
const char* health_status_str(health_status_t status) {
    switch (status) {
        case HEALTH_UP:
            return "UP";
        case HEALTH_DEGRADED:
            return "DEGRADED";
        case HEALTH_DOWN:
            return "DOWN";
        default:
            return "UNKNOWN";
    }
}

/* Initialize health registry */
distric_err_t health_init(health_registry_t** registry) {
    if (!registry) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    health_registry_t* reg = calloc(1, sizeof(health_registry_t));
    if (!reg) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    atomic_init(&reg->component_count, 0);
    
    if (pthread_mutex_init(&reg->lock, NULL) != 0) {
        free(reg);
        return DISTRIC_ERR_INIT_FAILED;
    }
    
    /* Initialize all components as inactive */
    for (size_t i = 0; i < MAX_HEALTH_COMPONENTS; i++) {
        atomic_init(&reg->components[i].active, false);
        atomic_init(&reg->components[i].status, HEALTH_UP);
    }
    
    *registry = reg;
    return DISTRIC_OK;
}

/* Destroy health registry */
void health_destroy(health_registry_t* registry) {
    if (registry) {
        pthread_mutex_destroy(&registry->lock);
        free(registry);
    }
}

/* Register health check component */
distric_err_t health_register_component(health_registry_t* registry,
                                        const char* name,
                                        health_component_t** out_component) {
    if (!registry || !name || !out_component) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&registry->lock);
    
    /* Check if component already exists */
    size_t count = atomic_load(&registry->component_count);
    for (size_t i = 0; i < count; i++) {
        health_component_t* comp = &registry->components[i];
        if (atomic_load(&comp->active) && strcmp(comp->name, name) == 0) {
            pthread_mutex_unlock(&registry->lock);
            *out_component = comp;
            return DISTRIC_OK;
        }
    }
    
    /* Allocate new component */
    if (count >= MAX_HEALTH_COMPONENTS) {
        pthread_mutex_unlock(&registry->lock);
        return DISTRIC_ERR_REGISTRY_FULL;
    }
    
    health_component_t* comp = &registry->components[count];
    
    strncpy(comp->name, name, MAX_COMPONENT_NAME_LEN - 1);
    comp->name[MAX_COMPONENT_NAME_LEN - 1] = '\0';
    
    atomic_store(&comp->status, HEALTH_UP);
    strncpy(comp->message, "Component initialized", MAX_HEALTH_MESSAGE_LEN - 1);
    comp->message[MAX_HEALTH_MESSAGE_LEN - 1] = '\0';
    
    comp->last_check_time_ms = get_timestamp_ms();
    atomic_store(&comp->active, true);
    
    atomic_store(&registry->component_count, count + 1);
    
    pthread_mutex_unlock(&registry->lock);
    
    *out_component = comp;
    return DISTRIC_OK;
}

/* Update component health status */
distric_err_t health_update_status(health_component_t* component,
                                   health_status_t status,
                                   const char* message) {
    if (!component) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    atomic_store(&component->status, status);
    
    if (message) {
        strncpy(component->message, message, MAX_HEALTH_MESSAGE_LEN - 1);
        component->message[MAX_HEALTH_MESSAGE_LEN - 1] = '\0';
    }
    
    component->last_check_time_ms = get_timestamp_ms();
    
    return DISTRIC_OK;
}

/* Get overall system health */
health_status_t health_get_overall_status(health_registry_t* registry) {
    if (!registry) {
        return HEALTH_DOWN;
    }
    
    health_status_t worst = HEALTH_UP;
    size_t count = atomic_load(&registry->component_count);
    
    for (size_t i = 0; i < count; i++) {
        health_component_t* comp = &registry->components[i];
        if (!atomic_load(&comp->active)) {
            continue;
        }
        
        health_status_t status = atomic_load(&comp->status);
        if (status > worst) {
            worst = status;
        }
    }
    
    return worst;
}

/* Escape JSON string */
static void json_escape(const char* src, char* dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j < dst_size - 3) dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 'n';
            }
        } else if ((unsigned char)c < 32) {
            continue;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* Export health status as JSON */
distric_err_t health_export_json(health_registry_t* registry,
                                 char** out_buffer,
                                 size_t* out_size) {
    if (!registry || !out_buffer || !out_size) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t buffer_size = 64 * 1024;  /* 64KB */
    char* buffer = malloc(buffer_size);
    if (!buffer) {
        return DISTRIC_ERR_ALLOC_FAILURE;
    }
    
    size_t offset = 0;
    
    /* Get overall status */
    health_status_t overall = health_get_overall_status(registry);
    
    /* Start JSON object */
    int written = snprintf(buffer + offset, buffer_size - offset,
                          "{\"status\":\"%s\",\"components\":[",
                          health_status_str(overall));
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        free(buffer);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    
    /* Add components */
    size_t count = atomic_load(&registry->component_count);
    bool first = true;
    
    for (size_t i = 0; i < count; i++) {
        health_component_t* comp = &registry->components[i];
        if (!atomic_load(&comp->active)) {
            continue;
        }
        
        char escaped_message[MAX_HEALTH_MESSAGE_LEN * 2];
        json_escape(comp->message, escaped_message, sizeof(escaped_message));
        
        health_status_t status = atomic_load(&comp->status);
        
        written = snprintf(buffer + offset, buffer_size - offset,
                          "%s{\"name\":\"%s\",\"status\":\"%s\",\"message\":\"%s\",\"lastCheck\":%lu}",
                          first ? "" : ",",
                          comp->name,
                          health_status_str(status),
                          escaped_message,
                          comp->last_check_time_ms);
        
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            free(buffer);
            return DISTRIC_ERR_BUFFER_OVERFLOW;
        }
        offset += written;
        first = false;
    }
    
    /* Close JSON */
    written = snprintf(buffer + offset, buffer_size - offset, "]}");
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        free(buffer);
        return DISTRIC_ERR_BUFFER_OVERFLOW;
    }
    offset += written;
    
    *out_buffer = buffer;
    *out_size = offset;
    
    return DISTRIC_OK;
}