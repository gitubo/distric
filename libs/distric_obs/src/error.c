#include "distric_obs.h"

/* Convert error code to human-readable string */
const char* distric_strerror(distric_err_t err) {
    switch (err) {
        case DISTRIC_OK:
            return "Success";
        case DISTRIC_ERR_INVALID_ARG:
            return "Invalid argument";
        case DISTRIC_ERR_ALLOC_FAILURE:
            return "Memory allocation failed";
        case DISTRIC_ERR_BUFFER_OVERFLOW:
            return "Buffer overflow";
        case DISTRIC_ERR_REGISTRY_FULL:
            return "Registry is full";
        case DISTRIC_ERR_NOT_FOUND:
            return "Not found";
        case DISTRIC_ERR_INIT_FAILED:
            return "Initialization failed";
        default:
            return "Unknown error";
    }
}