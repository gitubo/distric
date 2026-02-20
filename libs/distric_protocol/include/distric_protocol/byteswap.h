/**
 * @file byteswap.h
 * @brief Portable 64-bit byte-swap helpers (internal, not part of public API)
 *
 * Improvement #1 — Eliminate htonll/ntohll duplication between binary.c and
 * tlv.c.  A single definition here is the authoritative source; both
 * translation units include this header.
 *
 * Design notes:
 *  - Uses __builtin_bswap64 as a fast path when available (GCC ≥ 4.3,
 *    Clang ≥ 3.0).  This compiles to a single BSWAP instruction on x86-64.
 *  - Falls back to a htonl-based implementation for older compilers /
 *    cross-compilation targets.
 *  - A _Static_assert guards against exotic ABI targets where uint64_t is
 *    not exactly 8 bytes.
 *  - The header is intentionally placed under src/ (not include/) so it
 *    cannot be included by library consumers.
 */

#ifndef DISTRIC_PROTOCOL_INTERNAL_BYTESWAP_H
#define DISTRIC_PROTOCOL_INTERNAL_BYTESWAP_H

#include <stdint.h>
#include <arpa/inet.h>   /* htonl, ntohl */

_Static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");

/* ============================================================================
 * htonll / ntohll — host ↔ network (big-endian) for 64-bit values
 * ========================================================================= */

#ifndef htonll
# if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

static inline uint64_t htonll(uint64_t v) { return v; }
static inline uint64_t ntohll(uint64_t v) { return v; }

# elif defined(__GNUC__) || defined(__clang__)

static inline uint64_t htonll(uint64_t v)
{
    return (uint64_t)__builtin_bswap64((uint64_t)v);
}
static inline uint64_t ntohll(uint64_t v) { return htonll(v); }

# else /* portable fallback via htonl */

static inline uint64_t htonll(uint64_t v)
{
    return ((uint64_t)htonl((uint32_t)(v & 0xFFFFFFFFu)) << 32)
         | ((uint64_t)htonl((uint32_t)(v >> 32)));
}
static inline uint64_t ntohll(uint64_t v) { return htonll(v); }

# endif
#endif /* htonll */

#endif /* DISTRIC_PROTOCOL_INTERNAL_BYTESWAP_H */