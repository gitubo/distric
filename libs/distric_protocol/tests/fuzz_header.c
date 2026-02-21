/*
 * fuzz_header.c — libFuzzer harness for message header parsing + CRC verify
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -O1 -I../include \
 *         fuzz_header.c ../src/binary.c ../src/crc32.c \
 *         ../src/error_info.c -o fuzz_header
 *
 * Run:
 *   ./fuzz_header -max_len=128 corpus/
 */

#include "distric_protocol/binary.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Need at least a full header */
    if (size < MESSAGE_HEADER_SIZE) return 0;

    message_header_t header;
    if (deserialize_header(data, &header) != DISTRIC_OK) return 0;

    validate_message_header(&header);

    const uint8_t *payload     = data + MESSAGE_HEADER_SIZE;
    size_t         payload_len = size - MESSAGE_HEADER_SIZE;

    verify_message_crc32(&header, payload, payload_len);

    return 0;
}