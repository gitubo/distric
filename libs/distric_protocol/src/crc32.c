/**
 * @file crc32.c
 * @brief CRC32 Checksum Implementation
 *
 * Table-based CRC32 using IEEE 802.3 polynomial (0xEDB88320 reversed).
 *
 * Fix #2 — Thread-safe lazy initialisation:
 *   Replaced the hand-rolled atomic-bool guard (which had a data race: the 256
 *   table writes were not ordered relative to the flag store visible to other
 *   threads) with pthread_once(3).  pthread_once provides the required
 *   happens-before guarantee: all table writes complete and are visible before
 *   any subsequent reader proceeds.
 *
 *   Alternatively, the table could be generated at compile time as a static
 *   const array (zero runtime overhead, zero race risk).  pthread_once is
 *   chosen here to minimise diff size and remain compatible with the existing
 *   public crc32_init_table() call site.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/crc32.h"
#include <pthread.h>

/* ============================================================================
 * CRC32 LOOKUP TABLE
 * ========================================================================= */

/** CRC32 lookup table (256 entries, IEEE 802.3 / Ethernet polynomial) */
static uint32_t crc32_table[256];

/** pthread_once control — guarantees single, race-free initialisation */
static pthread_once_t crc32_once = PTHREAD_ONCE_INIT;

/**
 * @brief Internal one-shot table builder invoked by pthread_once.
 *
 * Uses the reflected (LSB-first) IEEE 802.3 polynomial 0xEDB88320
 * (the bit-reversal of 0x04C11DB7).
 */
static void crc32_build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
}

/* ============================================================================
 * PUBLIC INITIALISATION
 * ========================================================================= */

/**
 * @brief Ensure CRC32 lookup table is initialised.
 *
 * Safe to call from multiple threads simultaneously.  Subsequent calls after
 * the first are no-ops (pthread_once cost: a single atomic load).
 * Can be called manually for deterministic initialisation timing, but all
 * compute_crc32*() functions call it automatically.
 */
void crc32_init_table(void)
{
    pthread_once(&crc32_once, crc32_build_table);
}

/* ============================================================================
 * CRC32 COMPUTATION
 * ========================================================================= */

/**
 * @brief Compute CRC32 over a data chunk with a caller-supplied running value.
 *
 * Allows streaming computation across multiple chunks:
 * @code
 *   uint32_t crc = 0xFFFFFFFF;
 *   crc = compute_crc32_incremental(chunk1, len1, crc);
 *   crc = compute_crc32_incremental(chunk2, len2, crc);
 *   uint32_t final = ~crc;
 * @endcode
 */
uint32_t compute_crc32_incremental(const void* data, size_t len, uint32_t crc)
{
    if (!data || len == 0) {
        return crc;
    }

    /* Guarantee table is ready (idempotent after first call) */
    pthread_once(&crc32_once, crc32_build_table);

    const uint8_t* ptr = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc ^ ptr[i]) & 0xFFu);
        crc = (crc >> 8) ^ crc32_table[idx];
    }

    return crc;
}

/**
 * @brief Compute CRC32 checksum over an entire buffer.
 *
 * Equivalent to:
 *   ~compute_crc32_incremental(data, len, 0xFFFFFFFF)
 *
 * @return 0 for NULL/empty input; CRC32 otherwise.
 */
uint32_t compute_crc32(const void* data, size_t len)
{
    if (!data || len == 0) {
        return 0;
    }

    uint32_t crc = compute_crc32_incremental(data, len, 0xFFFFFFFFu);
    return ~crc;
}