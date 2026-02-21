/*
 * fuzz_tlv.c — libFuzzer harness for TLV decoder
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -O1 -I../include \
 *         fuzz_tlv.c ../src/tlv.c ../src/error_info.c -o fuzz_tlv
 *
 * Run:
 *   ./fuzz_tlv -max_len=65536 corpus/
 */

#include "distric_protocol/tlv.h"
#include "distric_protocol/limits.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;

    /* --- strict limits to keep fuzzer fast --- */
    protocol_limits_t limits = PROTOCOL_LIMITS_STRICT;

    /* --- borrowed decode path --- */
    {
        tlv_decoder_t *dec = tlv_decoder_create_with_limits(data, size, &limits);
        if (dec) {
            tlv_field_t field;
            while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
                /* consume value to exercise extractor paths */
                switch (field.type) {
                case TLV_UINT32: tlv_field_get_uint32(&field); break;
                case TLV_UINT64: tlv_field_get_uint64(&field); break;
                case TLV_STRING: tlv_field_get_string(&field); break;
                default: break;
                }
            }
            tlv_decoder_free(dec);
        }
    }

    /* --- owned decode path --- */
    {
        tlv_decoder_t *dec = tlv_decoder_create_with_limits(data, size, &limits);
        if (dec) {
            tlv_field_owned_t field;
            while (tlv_decode_next_owned(dec, &field) == DISTRIC_OK) {
                tlv_field_owned_free(&field);
            }
            tlv_decoder_free(dec);
        }
    }

    /* --- indexed decoder path --- */
    {
        tlv_decoder_t *dec = tlv_decoder_create_with_limits(data, size, &limits);
        if (dec) {
            if (tlv_decoder_build_index(dec) == DISTRIC_OK) {
                tlv_field_t field;
                /* probe a few arbitrary tags */
                tlv_decoder_find_indexed(dec, 0x0001, &field);
                tlv_decoder_find_indexed(dec, 0x0100, &field);
                tlv_decoder_find_indexed(dec, 0xFFFF, &field);
            }
            tlv_decoder_free(dec);
        }
    }

    /* --- validation-only path --- */
    tlv_validate_buffer(data, size);

    return 0;
}