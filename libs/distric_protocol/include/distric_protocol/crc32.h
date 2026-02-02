/**
 * @file crc32.h
 * @brief CRC32 Checksum Computation
 * 
 * Fast table-based CRC32 implementation using the IEEE 802.3 polynomial.
 * 
 * Polynomial: 0x04C11DB7 (reversed: 0xEDB88320)
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_PROTOCOL_CRC32_H
#define DISTRIC_PROTOCOL_CRC32_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CRC32 API
 * ========================================================================= */

/**
 * @brief Compute CRC32 checksum over data
 * 
 * Uses table-based algorithm for fast computation.
 * Polynomial: 0xEDB88320 (IEEE 802.3, reversed)
 * 
 * @param data Input data
 * @param len Data length in bytes
 * @return CRC32 checksum
 */
uint32_t compute_crc32(const void* data, size_t len);

/**
 * @brief Compute CRC32 with initial value (for incremental computation)
 * 
 * Allows computing CRC32 over multiple data chunks:
 * 
 * @code
 * uint32_t crc = 0xFFFFFFFF;
 * crc = compute_crc32_incremental(chunk1, len1, crc);
 * crc = compute_crc32_incremental(chunk2, len2, crc);
 * crc = ~crc;  // Final inversion
 * @endcode
 * 
 * @param data Input data
 * @param len Data length in bytes
 * @param crc Initial CRC value
 * @return Updated CRC32 value
 */
uint32_t compute_crc32_incremental(const void* data, size_t len, uint32_t crc);

/**
 * @brief Initialize CRC32 lookup table
 * 
 * This function is called automatically on first use.
 * Can be called manually for predictable initialization timing.
 */
void crc32_init_table(void);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_CRC32_H */