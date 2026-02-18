/**
 * @file health_internal.h
 * @brief DistriC Observability — Health Monitoring Internal Details
 *
 * NOT part of the public API.  Only health.c may include this header.
 */

#ifndef DISTRIC_HEALTH_INTERNAL_H
#define DISTRIC_HEALTH_INTERNAL_H

#include "distric_obs.h"
#include <stdatomic.h>
#include <pthread.h>

#define MAX_HEALTH_COMPONENTS  DISTRIC_MAX_HEALTH_COMPONENTS
#define MAX_COMPONENT_NAME_LEN 64
#define MAX_HEALTH_MESSAGE_LEN 256

struct health_component_s {
    char             name[MAX_COMPONENT_NAME_LEN];
    _Atomic int      status;                    /* health_status_t */
    char             message[MAX_HEALTH_MESSAGE_LEN];
    pthread_mutex_t  message_lock;              /* protects message field */
    uint64_t         last_check_time_ms;
    _Atomic bool     active;
};

struct health_registry_s {
    health_component_t components[MAX_HEALTH_COMPONENTS];
    _Atomic size_t     component_count;
    pthread_mutex_t    register_lock;
    _Atomic uint32_t   refcount;
};

#ifdef NDEBUG
#define HEALTH_ASSERT_LIFECYCLE(cond) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define HEALTH_ASSERT_LIFECYCLE(cond)                                       \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[distric_obs] HEALTH LIFECYCLE VIOLATION: %s " \
                    "(%s:%d)\n", #cond, __FILE__, __LINE__);                  \
            abort();                                                           \
        }                                                                     \
    } while (0)
#endif

#endif /* DISTRIC_HEALTH_INTERNAL_H */