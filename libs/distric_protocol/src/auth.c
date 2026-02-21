#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/auth.h"
#include "distric_protocol/error_info.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ============================================================================
 * SHA-256 (pure C11, no external dependency)
 * ========================================================================= */

#define ROR32(v, n) (((v) >> (n)) | ((v) << (32u - (n))))

static const uint32_t SHA256_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

typedef struct {
    uint32_t h[8];
    uint64_t bit_len;
    uint8_t  buf[64];
    uint32_t buf_used;
} sha256_ctx_t;

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->h[0]=0x6a09e667u; ctx->h[1]=0xbb67ae85u;
    ctx->h[2]=0x3c6ef372u; ctx->h[3]=0xa54ff53au;
    ctx->h[4]=0x510e527fu; ctx->h[5]=0x9b05688cu;
    ctx->h[6]=0x1f83d9abu; ctx->h[7]=0x5be0cd19u;
    ctx->bit_len=0; ctx->buf_used=0;
}

static void sha256_compress(sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (int i=0;i<16;i++) {
        w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)
            |((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
    }
    for (int i=16;i<64;i++) {
        uint32_t s0=ROR32(w[i-15],7)^ROR32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ROR32(w[i-2],17)^ROR32(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=ctx->h[0],b=ctx->h[1],c=ctx->h[2],d=ctx->h[3];
    uint32_t e=ctx->h[4],f=ctx->h[5],g=ctx->h[6],h=ctx->h[7];
    for (int i=0;i<64;i++) {
        uint32_t S1=ROR32(e,6)^ROR32(e,11)^ROR32(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+SHA256_K[i]+w[i];
        uint32_t S0=ROR32(a,2)^ROR32(a,13)^ROR32(a,22);
        uint32_t mj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx->h[0]+=a;ctx->h[1]+=b;ctx->h[2]+=c;ctx->h[3]+=d;
    ctx->h[4]+=e;ctx->h[5]+=f;ctx->h[6]+=g;ctx->h[7]+=h;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    while (len>0) {
        uint32_t room=64u-ctx->buf_used;
        uint32_t take=(len<room)?(uint32_t)len:room;
        memcpy(ctx->buf+ctx->buf_used, data, take);
        ctx->buf_used+=take; ctx->bit_len+=(uint64_t)take*8u;
        data+=take; len-=take;
        if (ctx->buf_used==64u) { sha256_compress(ctx,ctx->buf); ctx->buf_used=0; }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    ctx->buf[ctx->buf_used++]=0x80u;
    if (ctx->buf_used>56u) {
        while (ctx->buf_used<64u) ctx->buf[ctx->buf_used++]=0;
        sha256_compress(ctx,ctx->buf); ctx->buf_used=0;
    }
    while (ctx->buf_used<56u) ctx->buf[ctx->buf_used++]=0;
    uint64_t bl=ctx->bit_len;
    ctx->buf[56]=(uint8_t)(bl>>56);ctx->buf[57]=(uint8_t)(bl>>48);
    ctx->buf[58]=(uint8_t)(bl>>40);ctx->buf[59]=(uint8_t)(bl>>32);
    ctx->buf[60]=(uint8_t)(bl>>24);ctx->buf[61]=(uint8_t)(bl>>16);
    ctx->buf[62]=(uint8_t)(bl>>8); ctx->buf[63]=(uint8_t)(bl);
    sha256_compress(ctx,ctx->buf);
    for (int i=0;i<8;i++) {
        out[i*4+0]=(uint8_t)(ctx->h[i]>>24);
        out[i*4+1]=(uint8_t)(ctx->h[i]>>16);
        out[i*4+2]=(uint8_t)(ctx->h[i]>>8);
        out[i*4+3]=(uint8_t)(ctx->h[i]);
    }
}

/* ============================================================================
 * HMAC-SHA256 — two-chunk incremental, zero allocation
 * ========================================================================= */

static void hmac_sha256_2chunk(
    const uint8_t *key,    size_t key_len,
    const uint8_t *chunk1, size_t len1,
    const uint8_t *chunk2, size_t len2,
    uint8_t        out[32])
{
    uint8_t k_pad[64], k_norm[32];

    if (key_len > 64u) {
        sha256_ctx_t c; sha256_init(&c);
        sha256_update(&c, key, key_len);
        sha256_final(&c, k_norm);
        key=k_norm; key_len=32u;
    }

    /* inner */
    memset(k_pad, 0x36u, 64u);
    for (size_t i=0;i<key_len;i++) k_pad[i]^=key[i];
    sha256_ctx_t inner; sha256_init(&inner);
    sha256_update(&inner, k_pad, 64u);
    if (chunk1 && len1) sha256_update(&inner, chunk1, len1);
    if (chunk2 && len2) sha256_update(&inner, chunk2, len2);
    uint8_t ih[32]; sha256_final(&inner, ih);

    /* outer */
    memset(k_pad, 0x5cu, 64u);
    for (size_t i=0;i<key_len;i++) k_pad[i]^=key[i];
    sha256_ctx_t outer; sha256_init(&outer);
    sha256_update(&outer, k_pad, 64u);
    sha256_update(&outer, ih, 32u);
    sha256_final(&outer, out);
}

void distric_hmac_sha256(
    const uint8_t *key,  size_t key_len,
    const uint8_t *data, size_t data_len,
    uint8_t        out[DISTRIC_AUTH_MAC_SIZE])
{
    hmac_sha256_2chunk(key, key_len, data, data_len, NULL, 0, out);
}

/* ============================================================================
 * AUTH API
 * ========================================================================= */

distric_err_t distric_auth_compute(
    const distric_auth_ctx_t *ctx,
    const uint8_t            *header_buf,
    const uint8_t            *payload,
    size_t                    payload_len,
    uint8_t                   mac_out[DISTRIC_AUTH_MAC_SIZE])
{
    if (!ctx || !header_buf || !mac_out) return DISTRIC_ERR_INVALID_ARG;
    if (ctx->key_len == 0) { memset(mac_out,0,DISTRIC_AUTH_MAC_SIZE); return DISTRIC_OK; }

    if (ctx->mac_fn) {
        /* Pluggable path: needs contiguous buffer */
        size_t total = 32u + payload_len;
        if (total <= 512u) {
            uint8_t sbuf[512];
            memcpy(sbuf, header_buf, 32u);
            if (payload && payload_len) memcpy(sbuf+32u, payload, payload_len);
            ctx->mac_fn(ctx->key, ctx->key_len, sbuf, total, mac_out);
        } else {
            uint8_t *hbuf = (uint8_t*)malloc(total);
            if (!hbuf) return DISTRIC_ERR_NO_MEMORY;
            memcpy(hbuf, header_buf, 32u);
            if (payload && payload_len) memcpy(hbuf+32u, payload, payload_len);
            ctx->mac_fn(ctx->key, ctx->key_len, hbuf, total, mac_out);
            free(hbuf);
        }
    } else {
        /* Built-in path: two-chunk, zero extra allocation */
        hmac_sha256_2chunk(ctx->key, ctx->key_len,
                           header_buf, 32u,
                           payload,    payload_len,
                           mac_out);
    }
    return DISTRIC_OK;
}

distric_err_t distric_auth_verify(
    const distric_auth_ctx_t *ctx,
    const uint8_t            *header_buf,
    const uint8_t            *payload,
    size_t                    payload_len,
    const uint8_t             expected_mac[DISTRIC_AUTH_MAC_SIZE])
{
    if (!ctx || !header_buf || !expected_mac) return DISTRIC_ERR_INVALID_ARG;
    if (ctx->key_len == 0) return DISTRIC_OK;

    uint8_t computed[DISTRIC_AUTH_MAC_SIZE];
    distric_err_t err = distric_auth_compute(ctx, header_buf, payload,
                                              payload_len, computed);
    if (err != DISTRIC_OK) return err;

    volatile uint8_t diff = 0;
    for (size_t i=0; i<DISTRIC_AUTH_MAC_SIZE; i++)
        diff |= computed[i] ^ expected_mac[i];

    if (diff != 0) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT,
                               PROTO_STAGE_AUTH_VERIFY, 0, 0, "HMAC mismatch");
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    return DISTRIC_OK;
}