/*
 * error.c — DistriC Observability Library — Error Handling
 */

#include "distric_obs.h"

const char* distric_err_str(distric_err_t err) {
    switch (err) {
        case DISTRIC_OK:                   return "OK";
        case DISTRIC_ERR_INVALID_ARG:      return "invalid argument";
        case DISTRIC_ERR_ALLOC_FAILURE:    return "allocation failure";
        case DISTRIC_ERR_INIT_FAILED:      return "initialization failed";
        case DISTRIC_ERR_REGISTRY_FULL:    return "registry full";
        case DISTRIC_ERR_NOT_FOUND:        return "not found";
        case DISTRIC_ERR_BUFFER_OVERFLOW:  return "buffer overflow (data dropped)";
        case DISTRIC_ERR_BACKPRESSURE:     return "backpressure (span dropped)";
        case DISTRIC_ERR_INVALID_LABEL:    return "invalid label value";
        case DISTRIC_ERR_HIGH_CARDINALITY: return "label cardinality too high";
        case DISTRIC_ERR_REGISTRY_FROZEN:  return "registry frozen";
        case DISTRIC_ERR_NO_MEMORY:        return "no memory (update dropped)";
        case DISTRIC_ERR_ALREADY_EXISTS:   return "already exists";
        case DISTRIC_ERR_SHUTDOWN:         return "subsystem shutdown";
        default:                           return "unknown error";
    }
}