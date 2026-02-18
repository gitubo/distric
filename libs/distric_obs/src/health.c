/*
 * health.c — DistriC Observability Library — Health Monitoring
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "distric_obs.h"
#include "distric_obs/health.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <pthread.h>

static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

const char* health_status_str(health_status_t status) {
    switch (status) {
        case HEALTH_UP:       return "UP";
        case HEALTH_DEGRADED: return "DEGRADED";
        case HEALTH_DOWN:     return "DOWN";
        default:              return "UNKNOWN";
    }
}

distric_err_t health_init(health_registry_t** registry) {
    if (!registry) return DISTRIC_ERR_INVALID_ARG;

    health_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) return DISTRIC_ERR_ALLOC_FAILURE;

    atomic_init(&reg->component_count, 0);
    atomic_init(&reg->refcount, 1);

    if (pthread_mutex_init(&reg->register_lock, NULL) != 0) {
        free(reg);
        return DISTRIC_ERR_INIT_FAILED;
    }

    for (size_t i = 0; i < MAX_HEALTH_COMPONENTS; i++) {
        atomic_init(&reg->components[i].status, (int)HEALTH_UP);
        atomic_init(&reg->components[i].active, false);
        pthread_mutex_init(&reg->components[i].message_lock, NULL);
    }

    *registry = reg;
    return DISTRIC_OK;
}

void health_destroy(health_registry_t* registry) {
    if (!registry) return;
    for (size_t i = 0; i < MAX_HEALTH_COMPONENTS; i++)
        pthread_mutex_destroy(&registry->components[i].message_lock);
    pthread_mutex_destroy(&registry->register_lock);
    free(registry);
}

distric_err_t health_register_component(health_registry_t*   registry,
                                          const char*          name,
                                          health_component_t** out_component) {
    if (!registry || !name || !out_component) return DISTRIC_ERR_INVALID_ARG;

    pthread_mutex_lock(&registry->register_lock);

    /* Return existing component if already registered under this name */
    size_t count = atomic_load_explicit(&registry->component_count,
                                        memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        if (strncmp(registry->components[i].name, name,
                    MAX_COMPONENT_NAME_LEN) == 0) {
            *out_component = &registry->components[i];
            pthread_mutex_unlock(&registry->register_lock);
            return DISTRIC_OK;
        }
    }

    size_t idx = count;
    if (idx >= MAX_HEALTH_COMPONENTS) {
        pthread_mutex_unlock(&registry->register_lock);
        return DISTRIC_ERR_REGISTRY_FULL;
    }

    health_component_t* comp = &registry->components[idx];
    strncpy(comp->name, name, MAX_COMPONENT_NAME_LEN - 1);
    comp->name[MAX_COMPONENT_NAME_LEN - 1] = '\0';
    atomic_store_explicit(&comp->status, (int)HEALTH_UP, memory_order_relaxed);
    comp->last_check_time_ms = get_timestamp_ms();
    atomic_store_explicit(&comp->active, true, memory_order_release);

    atomic_store_explicit(&registry->component_count, idx + 1, memory_order_release);
    pthread_mutex_unlock(&registry->register_lock);

    *out_component = comp;
    return DISTRIC_OK;
}

distric_err_t health_update_status(health_component_t* component,
                                    health_status_t     status,
                                    const char*         message) {
    if (!component) return DISTRIC_ERR_INVALID_ARG;

    HEALTH_ASSERT_LIFECYCLE(
        atomic_load_explicit(&component->active, memory_order_relaxed));

    atomic_store_explicit(&component->status, (int)status, memory_order_release);
    component->last_check_time_ms = get_timestamp_ms();

    if (message) {
        pthread_mutex_lock(&component->message_lock);
        strncpy(component->message, message, MAX_HEALTH_MESSAGE_LEN - 1);
        component->message[MAX_HEALTH_MESSAGE_LEN - 1] = '\0';
        pthread_mutex_unlock(&component->message_lock);
    }

    return DISTRIC_OK;
}

health_status_t health_get_overall_status(health_registry_t* registry) {
    if (!registry) return HEALTH_DOWN;

    health_status_t worst = HEALTH_UP;
    size_t n = atomic_load_explicit(&registry->component_count, memory_order_acquire);
    for (size_t i = 0; i < n; i++) {
        health_component_t* c = &registry->components[i];
        if (!atomic_load_explicit(&c->active, memory_order_acquire)) continue;
        int s = atomic_load_explicit(&c->status, memory_order_relaxed);
        if (s > (int)worst) worst = (health_status_t)s;
    }
    return worst;
}

distric_err_t health_export_json(health_registry_t* registry,
                                   char** out_json, size_t* out_size) {
    if (!registry || !out_json || !out_size) return DISTRIC_ERR_INVALID_ARG;

    size_t n = atomic_load_explicit(&registry->component_count, memory_order_acquire);

    /* Estimate buffer size */
    size_t buf_size = 256 + n * (MAX_COMPONENT_NAME_LEN + MAX_HEALTH_MESSAGE_LEN + 128);
    char*  buf = malloc(buf_size);
    if (!buf) return DISTRIC_ERR_NO_MEMORY;

    health_status_t overall = health_get_overall_status(registry);
    size_t offset = 0;
    int w;

#define JAPPEND(fmt, ...) \
    do { w = snprintf(buf + offset, buf_size - offset, fmt, ##__VA_ARGS__); \
         if (w > 0) offset += (size_t)w; } while (0)

    JAPPEND("{\"status\":\"%s\",\"components\":[",
            health_status_str(overall));

    for (size_t i = 0; i < n; i++) {
        health_component_t* c = &registry->components[i];
        if (!atomic_load_explicit(&c->active, memory_order_acquire)) continue;

        int s = atomic_load_explicit(&c->status, memory_order_relaxed);

        pthread_mutex_lock(&c->message_lock);
        char msg_copy[MAX_HEALTH_MESSAGE_LEN];
        strncpy(msg_copy, c->message, sizeof(msg_copy) - 1);
        msg_copy[sizeof(msg_copy) - 1] = '\0';
        pthread_mutex_unlock(&c->message_lock);

        JAPPEND("%s{\"name\":\"%s\",\"status\":\"%s\",\"message\":\"%s\","
                "\"last_check_ms\":%llu}",
                i ? "," : "",
                c->name,
                health_status_str((health_status_t)s),
                msg_copy,
                (unsigned long long)c->last_check_time_ms);
    }

    JAPPEND("]}");
#undef JAPPEND

    if (offset < buf_size) buf[offset] = '\0';
    *out_json = buf;
    *out_size = offset;
    return DISTRIC_OK;
}