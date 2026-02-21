#ifndef DISTRIC_PROTOCOL_INTERNAL_BYTESWAP_H
#define DISTRIC_PROTOCOL_INTERNAL_BYTESWAP_H

#include <stdint.h>
#include <arpa/inet.h>

_Static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");

#ifndef htonll
# if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline uint64_t htonll(uint64_t v) { return v; }
static inline uint64_t ntohll(uint64_t v) { return v; }
# elif defined(__GNUC__) || defined(__clang__)
static inline uint64_t htonll(uint64_t v) { return (uint64_t)__builtin_bswap64(v); }
static inline uint64_t ntohll(uint64_t v) { return htonll(v); }
# else
static inline uint64_t htonll(uint64_t v)
{
    return ((uint64_t)htonl((uint32_t)(v & 0xFFFFFFFFu)) << 32)
         | ((uint64_t)htonl((uint32_t)(v >> 32)));
}
static inline uint64_t ntohll(uint64_t v) { return htonll(v); }
# endif
#endif

#endif /* DISTRIC_PROTOCOL_INTERNAL_BYTESWAP_H */