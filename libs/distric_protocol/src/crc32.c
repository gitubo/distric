/**
 * @file crc32.c
 * @brief CRC32 Checksum Implementation
 * 
 * Table-based CRC32 using IEEE 802.3 polynomial (0xEDB88320 reversed).
 */

#include "distric_protocol/crc32.h"
#include <stdatomic.h>
#include <stdbool.h>

/* ============================================================================
 * CRC32 LOOKUP TABLE
 * ========================================================================= */

/** CRC32 lookup table (256 entries) */
static uint32_t crc32_table[256];

/** Table initialization flag */
static _Atomic bool table_initialized = false;

/**
 * @brief Initialize CRC32 lookup table
 * 
 * Uses IEEE 802.3 polynomial: 0xEDB88320 (reversed 0x04C11DB7)
 */
void crc32_init_table(void) {
    /* Check if already initialized (avoid redundant work) */
    if (atomic_load_explicit(&table_initialized, memory_order_acquire)) {
        return;
    }
    
    /* Generate lookup table */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc = crc >> 1;
            }
        }
        crc32_table[i] = crc;
    }
    
    /* Mark as initialized */
    atomic_store_explicit(&table_initialized, true, memory_order_release);
}

/* ============================================================================
 * CRC32 COMPUTATION
 * ========================================================================= */

/**
 * @brief Compute CRC32 with initial value
 */
uint32_t compute_crc32_incremental(const void* data, size_t len, uint32_t crc) {
    if (!data || len == 0) {
        return crc;
    }
    
    /* Ensure table is initialized */
    if (!atomic_load_explicit(&table_initialized, memory_order_acquire)) {
        crc32_init_table();
    }
    
    const uint8_t* ptr = (const uint8_t*)data;
    
    /* Process each byte */
    for (size_t i = 0; i < len; i++) {
        uint8_t index = (crc ^ ptr[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }
    
    return crc;
}

/**
 * @brief Compute CRC32 checksum over data
 */
uint32_t compute_crc32(const void* data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }
    
    /* Start with inverted initial value */
    uint32_t crc = 0xFFFFFFFF;
    
    /* Compute CRC */
    crc = compute_crc32_incremental(data, len, crc);
    
    /* Final inversion */
    return ~crc;
}