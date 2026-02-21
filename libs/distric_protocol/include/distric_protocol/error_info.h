#ifndef DISTRIC_PROTOCOL_ERROR_INFO_H
#define DISTRIC_PROTOCOL_ERROR_INFO_H

#include <stdint.h>
#include <distric_obs.h>   /* distric_err_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTO_STAGE_UNKNOWN              = 0,
    PROTO_STAGE_HEADER_DESERIALIZE   = 1,
    PROTO_STAGE_HEADER_VALIDATE      = 2,
    PROTO_STAGE_CRC_VERIFY           = 3,
    PROTO_STAGE_AUTH_VERIFY          = 4,
    PROTO_STAGE_LIMITS_CHECK         = 5,
    PROTO_STAGE_TLV_DECODE           = 6,
    PROTO_STAGE_MESSAGE_DESERIALIZE  = 7,
    PROTO_STAGE_CAPABILITY_NEGOTIATE = 8,
} protocol_stage_t;

/*
 * distric_error_info_t — rich error context attached to a protocol failure.
 *
 * Callers retrieve via distric_get_last_error() after any protocol function
 * returns non-DISTRIC_OK.  Storage is _Thread_local; no locking required.
 */
typedef struct {
    distric_err_t    code;
    protocol_stage_t stage;
    uint32_t         byte_offset;   /* buffer offset where error was detected */
    uint16_t         field_tag;     /* TLV tag involved (0 if not applicable)  */
    char             detail[128];   /* human-readable context string           */
} distric_error_info_t;

/* Retrieve last error info for the calling thread.  Never NULL. */
const distric_error_info_t *distric_get_last_error(void);

/* Clear last error for the calling thread. */
void distric_clear_error(void);

/*
 * Internal — used by protocol layer only.
 * Sets thread-local error info; safe to call from any thread.
 */
void distric_set_error_info(distric_err_t    code,
                             protocol_stage_t stage,
                             uint32_t         byte_offset,
                             uint16_t         field_tag,
                             const char      *detail);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_ERROR_INFO_H */