#ifndef DISTRIC_PROTOCOL_LIMITS_H
#define DISTRIC_PROTOCOL_LIMITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * protocol_limits_t — per-decode resource governance.
 *
 * Pass to tlv_decoder_create_with_limits() or validate_with_limits().
 * Zero fields use built-in production defaults.
 */
typedef struct {
    uint32_t max_fields;       /* max TLV fields iterated per message */
    uint32_t max_string_len;   /* max length of any STRING or BYTES field */
    uint32_t max_total_alloc;  /* max bytes heap-allocated by owned decode */
    uint32_t max_nesting;      /* reserved: future nested-TLV depth limit */
} protocol_limits_t;

/* ---------- built-in profiles ---------- */

/* Conservative defaults for production nodes */
#define PROTOCOL_LIMITS_DEFAULT  { 512u, (1u << 20), (16u << 20), 8u }

/* Tight limits for untrusted / public-facing endpoints */
#define PROTOCOL_LIMITS_STRICT   { 64u,  (64u * 1024u), (1u << 20), 4u }

/* Relaxed limits for trusted internal cluster traffic */
#define PROTOCOL_LIMITS_INTERNAL { 4096u, (16u << 20), (64u << 20), 16u }

/* ---------- accessors (handle NULL / zero gracefully) ---------- */

static inline uint32_t protocol_limits_max_fields(const protocol_limits_t *l)
{
    return (l && l->max_fields)     ? l->max_fields     : 512u;
}
static inline uint32_t protocol_limits_max_string(const protocol_limits_t *l)
{
    return (l && l->max_string_len) ? l->max_string_len : (1u << 20);
}
static inline uint32_t protocol_limits_max_alloc(const protocol_limits_t *l)
{
    return (l && l->max_total_alloc) ? l->max_total_alloc : (16u << 20);
}

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_LIMITS_H */