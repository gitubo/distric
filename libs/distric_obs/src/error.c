#include "distric_obs.h"

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
        case DISTRIC_ERR_NO_MEMORY:
            return "Out of memory";
        case DISTRIC_ERR_EOF:
            return "End of file";
        case DISTRIC_ERR_INVALID_FORMAT:
            return "Invalid format";
        case DISTRIC_ERR_TYPE_MISMATCH:
            return "Type mismatch";
        case DISTRIC_ERR_TIMEOUT:
            return "Timeout";
        case DISTRIC_ERR_MEMORY:
            return "Memory error";
        case DISTRIC_ERR_IO:
            return "I/O error";
        case DISTRIC_ERR_INVALID_STATE:
            return "Invalid state";
        case DISTRIC_ERR_THREAD:
            return "Thread error";
        case DISTRIC_ERR_UNAVAILABLE:
            return "Unavailable";
        case DISTRIC_ERR_REGISTRY_FROZEN:
            return "Registry is frozen";
        case DISTRIC_ERR_HIGH_CARDINALITY:
            return "High cardinality limit exceeded";
        case DISTRIC_ERR_INVALID_LABEL:
            return "Invalid label";
        case DISTRIC_ERR_BACKPRESSURE:
            return "Backpressure (item dropped)";
        default:
            return "Unknown error";
    }
}