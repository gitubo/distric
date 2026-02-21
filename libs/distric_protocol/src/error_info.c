#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/error_info.h"

#include <string.h>
#include <stdio.h>

/* Thread-local storage — C11, no mutex required */
static _Thread_local distric_error_info_t tl_last_error;
static _Thread_local bool                 tl_has_error;

const distric_error_info_t *distric_get_last_error(void)
{
    return &tl_last_error;
}

void distric_clear_error(void)
{
    tl_has_error = false;
    memset(&tl_last_error, 0, sizeof(tl_last_error));
}

void distric_set_error_info(distric_err_t    code,
                             protocol_stage_t stage,
                             uint32_t         byte_offset,
                             uint16_t         field_tag,
                             const char      *detail)
{
    tl_last_error.code        = code;
    tl_last_error.stage       = stage;
    tl_last_error.byte_offset = byte_offset;
    tl_last_error.field_tag   = field_tag;
    tl_has_error              = true;

    if (detail) {
        size_t n = sizeof(tl_last_error.detail) - 1u;
        strncpy(tl_last_error.detail, detail, n);
        tl_last_error.detail[n] = '\0';
    } else {
        tl_last_error.detail[0] = '\0';
    }
}