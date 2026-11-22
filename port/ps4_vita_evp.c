/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
 *
 * The part of OpenSSL's EVP interface Vita3K is written with, over Mbed TLS 2.16 (its AES, SHA-1 and
 * SHA-256; HMAC and CMAC are here). See port/vita-openssl/openssl/evp.h. Compiled into the Vita core
 * by scripts/build-vita-core.sh; checked against OpenSSL by local/herramientas/prueba-vita/evptest.
 */
#include <openssl/evp.h>

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <stdlib.h>
#include <string.h>

enum { MODE_ECB, MODE_CBC, MODE_CTR };
enum { MD_SHA1, MD_SHA256 };
enum { MAC_HMAC, MAC_CMAC };

struct evp_cipher_st {
    int mode;
    int key_bits;
};
struct evp_md_st {
    int kind;
    int size;
};
struct evp_mac_st {
    int kind;
};

static const EVP_CIPHER CIPHERS[] = {
    { MODE_ECB, 128 }, { MODE_ECB, 192 }, { MODE_ECB, 256 },
    { MODE_CBC, 128 }, { MODE_CBC, 192 }, { MODE_CBC, 256 },
    { MODE_CTR, 128 }, { MODE_CTR, 192 }, { MODE_CTR, 256 },
};
static const EVP_MD MD_OF_SHA1 = { MD_SHA1, 20 };
static const EVP_MD MD_OF_SHA256 = { MD_SHA256, 32 };
static const EVP_MAC MAC_OF_HMAC = { MAC_HMAC };
static const EVP_MAC MAC_OF_CMAC = { MAC_CMAC };

/* ---- ciphers ---- */

struct evp_cipher_ctx_st {
    mbedtls_aes_context aes;
    const EVP_CIPHER *cipher;
    int encrypt;
    int has_key;
    int padding;
    unsigned char iv[16];     /* CBC: the chain; CTR: the counter */
    unsigned char stream[16]; /* CTR: the key stream of the block in course */
    size_t stream_used;
    unsigned char rest[16];   /* ECB, CBC: the bytes of a block not whole yet */
    int rest_len;
};

EVP_CIPHER *EVP_CIPHER_fetch(OSSL_LIB_CTX *lib, const char *name, const char *properties) {
    static const char *const NAMES[] = {
        "AES-128-ECB", "AES-192-ECB", "AES-256-ECB", "AES-128-CBC", "AES-192-CBC", "AES-256-CBC",
        "AES-128-CTR", "AES-192-CTR", "AES-256-CTR",
    };
    (void)lib;
    (void)properties;
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (strcmp(name, NAMES[i]) == 0)
            return (EVP_CIPHER *)&CIPHERS[i];
    return NULL;
}
void EVP_CIPHER_free(EVP_CIPHER *cipher) { (void)cipher; }
const EVP_CIPHER *EVP_aes_128_ecb(void) { return &CIPHERS[0]; }
const EVP_CIPHER *EVP_aes_128_cbc(void) { return &CIPHERS[3]; }
const EVP_CIPHER *EVP_aes_128_ctr(void) { return &CIPHERS[6]; }

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void) {
    EVP_CIPHER_CTX *ctx = calloc(1, sizeof(*ctx));
    if (ctx) {
        mbedtls_aes_init(&ctx->aes);
        ctx->padding = 1;
    }
    return ctx;
}
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx) {
    if (!ctx)
        return;
    mbedtls_aes_free(&ctx->aes);
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}
int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int padding) {
    if (!ctx)
        return 0;
    ctx->padding = padding != 0;
    return 1;
}

static int cipher_init(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, const unsigned char *key,
                       const unsigned char *iv, int encrypt) {
    if (!ctx)
        return 0;
    if (cipher) {
        if (cipher != ctx->cipher)
            ctx->has_key = 0;
        ctx->cipher = cipher;
        ctx->padding = 1; /* (as OpenSSL: a new cipher, its padding on again) */
    }
    if (!ctx->cipher)
        return 0;
    if (ctx->has_key && ctx->encrypt != encrypt && !key && ctx->cipher->mode != MODE_CTR)
        return 0; /* (the key schedule of before is of the other direction) */
    ctx->encrypt = encrypt;
    if (key) {
        /* CTR runs AES forwards in both directions. */
        int result = (encrypt || ctx->cipher->mode == MODE_CTR)
                         ? mbedtls_aes_setkey_enc(&ctx->aes, key, (unsigned)ctx->cipher->key_bits)
                         : mbedtls_aes_setkey_dec(&ctx->aes, key, (unsigned)ctx->cipher->key_bits);
        if (result != 0)
            return 0;
        ctx->has_key = 1;
    }
    if (iv)
        memcpy(ctx->iv, iv, 16);
    else
        memset(ctx->iv, 0, 16);
    ctx->stream_used = 0;
    ctx->rest_len = 0;
    return 1;
}
int EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *engine,
                       const unsigned char *key, const unsigned char *iv) {
    (void)engine;
    return cipher_init(ctx, cipher, key, iv, 1);
}
int EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *engine,
                       const unsigned char *key, const unsigned char *iv) {
    (void)engine;
    return cipher_init(ctx, cipher, key, iv, 0);
}

static int cipher_blocks(EVP_CIPHER_CTX *ctx, unsigned char *out, const unsigned char *in, size_t size) {
    int direction = ctx->encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT;
    if (ctx->cipher->mode == MODE_CBC)
        return mbedtls_aes_crypt_cbc(&ctx->aes, direction, size, ctx->iv, in, out) == 0;
    for (size_t at = 0; at < size; at += 16)
        if (mbedtls_aes_crypt_ecb(&ctx->aes, direction, in + at, out + at) != 0)
            return 0;
    return 1;
}

static int cipher_update(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len, const unsigned char *in,
                         int in_len, int encrypt) {
    if (out_len)
        *out_len = 0;
    if (!ctx || !ctx->cipher || !ctx->has_key || ctx->encrypt != encrypt || in_len < 0 || !out_len)
        return 0;
    if (in_len == 0)
        return 1;
    if (!in || !out)
        return 0;
    if (ctx->cipher->mode == MODE_CTR) {
        if (mbedtls_aes_crypt_ctr(&ctx->aes, (size_t)in_len, &ctx->stream_used, ctx->iv, ctx->stream, in, out) != 0)
            return 0;
        *out_len = in_len;
        return 1;
    }
    size_t left = (size_t)in_len;
    size_t written = 0;
    if (ctx->rest_len > 0) {
        size_t take = 16 - (size_t)ctx->rest_len;
        if (take > left)
            take = left;
        memcpy(ctx->rest + ctx->rest_len, in, take);
        ctx->rest_len += (int)take;
        in += take;
        left -= take;
        if (ctx->rest_len < 16)
            return 1;
        unsigned char block[16];
        if (!cipher_blocks(ctx, block, ctx->rest, 16))
            return 0;
        memcpy(out, block, 16);
        out += 16;
        written = 16;
        ctx->rest_len = 0;
    }
    size_t whole = left & ~(size_t)15;
    if (whole > 0) {
        if (!cipher_blocks(ctx, out, in, whole))
            return 0;
        written += whole;
    }
    if (left > whole) {
        memcpy(ctx->rest, in + whole, left - whole);
        ctx->rest_len = (int)(left - whole);
    }
    *out_len = (int)written;
    return 1;
}
int EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len, const unsigned char *in, int in_len) {
    return cipher_update(ctx, out, out_len, in, in_len, 1);
}
int EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len, const unsigned char *in, int in_len) {
    return cipher_update(ctx, out, out_len, in, in_len, 0);
}

static int cipher_final(EVP_CIPHER_CTX *ctx, int *out_len, int encrypt) {
    if (out_len)
        *out_len = 0;
    if (!ctx || !ctx->cipher || !ctx->has_key || ctx->encrypt != encrypt)
        return 0;
    if (ctx->cipher->mode == MODE_CTR)
        return 1;
    return ctx->rest_len == 0 && !ctx->padding;
}
int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len) {
    (void)out;
    return cipher_final(ctx, out_len, 1);
}
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len) {
    (void)out;
    return cipher_final(ctx, out_len, 0);
}

/* ---- digests ---- */

struct evp_md_ctx_st {
    const EVP_MD *md; /* NULL: not started */
    union {
        mbedtls_sha1_context sha1;
        mbedtls_sha256_context sha256;
    } state;
};

EVP_MD *EVP_MD_fetch(OSSL_LIB_CTX *lib, const char *name, const char *properties) {
    (void)lib;
    (void)properties;
    if (!name)
        return NULL;
    if (strcmp(name, "SHA1") == 0 || strcmp(name, "SHA-1") == 0)
        return (EVP_MD *)&MD_OF_SHA1;
    if (strcmp(name, "SHA256") == 0 || strcmp(name, "SHA-256") == 0 || strcmp(name, "SHA2-256") == 0)
        return (EVP_MD *)&MD_OF_SHA256;
    return NULL;
}
void EVP_MD_free(EVP_MD *md) { (void)md; }
const EVP_MD *EVP_sha1(void) { return &MD_OF_SHA1; }
const EVP_MD *EVP_sha256(void) { return &MD_OF_SHA256; }

static void md_drop(EVP_MD_CTX *ctx) {
    if (!ctx->md)
        return;
    if (ctx->md->kind == MD_SHA1)
        mbedtls_sha1_free(&ctx->state.sha1);
    else
        mbedtls_sha256_free(&ctx->state.sha256);
    ctx->md = NULL;
}
static int md_start(EVP_MD_CTX *ctx, const EVP_MD *md) {
    md_drop(ctx);
    if (!md)
        return 0;
    int result;
    if (md->kind == MD_SHA1) {
        mbedtls_sha1_init(&ctx->state.sha1);
        result = mbedtls_sha1_starts_ret(&ctx->state.sha1);
    } else {
        mbedtls_sha256_init(&ctx->state.sha256);
        result = mbedtls_sha256_starts_ret(&ctx->state.sha256, 0);
    }
    ctx->md = md;
    return result == 0;
}
static int md_add(EVP_MD_CTX *ctx, const void *data, size_t size) {
    if (!ctx->md)
        return 0;
    if (size == 0)
        return 1;
    if (ctx->md->kind == MD_SHA1)
        return mbedtls_sha1_update_ret(&ctx->state.sha1, data, size) == 0;
    return mbedtls_sha256_update_ret(&ctx->state.sha256, data, size) == 0;
}
static int md_end(EVP_MD_CTX *ctx, unsigned char *out) {
    if (!ctx->md)
        return 0;
    int result = ctx->md->kind == MD_SHA1 ? mbedtls_sha1_finish_ret(&ctx->state.sha1, out)
                                          : mbedtls_sha256_finish_ret(&ctx->state.sha256, out);
    md_drop(ctx);
    return result == 0;
}

EVP_MD_CTX *EVP_MD_CTX_new(void) { return calloc(1, sizeof(EVP_MD_CTX)); }
void EVP_MD_CTX_free(EVP_MD_CTX *ctx) {
    if (!ctx)
        return;
    md_drop(ctx);
    free(ctx);
}
int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *md, ENGINE *engine) {
    (void)engine;
    return ctx && md_start(ctx, md);
}
int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *data, size_t size) { return ctx && md_add(ctx, data, size); }
int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *out, unsigned int *out_len) {
    if (!ctx || !ctx->md || !out)
        return 0;
    unsigned int size = (unsigned int)ctx->md->size;
    if (!md_end(ctx, out))
        return 0;
    if (out_len)
        *out_len = size;
    return 1;
}
int EVP_DigestFinal(EVP_MD_CTX *ctx, unsigned char *out, unsigned int *out_len) {
    return EVP_DigestFinal_ex(ctx, out, out_len);
}
int EVP_Digest(const void *data, size_t size, unsigned char *out, unsigned int *out_len, const EVP_MD *md,
               ENGINE *engine) {
    EVP_MD_CTX ctx;
    (void)engine;
    memset(&ctx, 0, sizeof(ctx));
    return md_start(&ctx, md) && md_add(&ctx, data, size) && EVP_DigestFinal_ex(&ctx, out, out_len);
}

/* ---- HMAC (RFC 2104) and AES-CMAC (RFC 4493) ---- */

struct evp_mac_ctx_st {
    int kind;
    int ready;
    /* HMAC */
    const EVP_MD *md;
    EVP_MD_CTX inner;
    unsigned char outer_pad[64];
    /* CMAC */
    mbedtls_aes_context aes;
    unsigned char chain[16];
    unsigned char last[16]; /* the last block isn't ciphered until the end is known */
    int last_len;
};

static int hmac_start(EVP_MAC_CTX *ctx, const EVP_MD *md, const unsigned char *key, size_t key_len) {
    unsigned char block[64]; /* SHA-1 and SHA-256: blocks of 64 bytes */
    unsigned char inner_pad[64];
    memset(block, 0, sizeof(block));
    if (key_len > sizeof(block)) {
        if (!EVP_Digest(key, key_len, block, NULL, md, NULL))
            return 0;
    } else if (key_len > 0) {
        memcpy(block, key, key_len);
    }
    for (int i = 0; i < 64; i++) {
        inner_pad[i] = block[i] ^ 0x36;
        ctx->outer_pad[i] = block[i] ^ 0x5c;
    }
    memset(block, 0, sizeof(block));
    ctx->md = md;
    return md_start(&ctx->inner, md) && md_add(&ctx->inner, inner_pad, 64);
}
static int hmac_end(EVP_MAC_CTX *ctx, unsigned char *out) {
    unsigned char inner[32];
    EVP_MD_CTX outer;
    memset(&outer, 0, sizeof(outer));
    if (!md_end(&ctx->inner, inner))
        return 0;
    return md_start(&outer, ctx->md) && md_add(&outer, ctx->outer_pad, 64) &&
           md_add(&outer, inner, (size_t)ctx->md->size) && md_end(&outer, out);
}

unsigned char *HMAC(const EVP_MD *md, const void *key, int key_len, const unsigned char *data, size_t size,
                    unsigned char *out, unsigned int *out_len) {
    EVP_MAC_CTX ctx;
    if (!md || !out || key_len < 0 || (key_len > 0 && !key))
        return NULL;
    memset(&ctx, 0, sizeof(ctx));
    if (!hmac_start(&ctx, md, key, (size_t)key_len) || !md_add(&ctx.inner, data, size) || !hmac_end(&ctx, out)) {
        md_drop(&ctx.inner);
        return NULL;
    }
    if (out_len)
        *out_len = (unsigned int)md->size;
    return out;
}

/* (The block times x in GF(2^128), for CMAC's two subkeys.) */
static void cmac_double(unsigned char *block) {
    unsigned char carry = block[0] >> 7;
    for (int i = 0; i < 15; i++)
        block[i] = (unsigned char)((block[i] << 1) | (block[i + 1] >> 7));
    block[15] = (unsigned char)((block[15] << 1) ^ (carry ? 0x87 : 0));
}
static int cmac_block(EVP_MAC_CTX *ctx, const unsigned char *block) {
    for (int i = 0; i < 16; i++)
        ctx->chain[i] ^= block[i];
    return mbedtls_aes_crypt_ecb(&ctx->aes, MBEDTLS_AES_ENCRYPT, ctx->chain, ctx->chain) == 0;
}
static int cmac_add(EVP_MAC_CTX *ctx, const unsigned char *data, size_t size) {
    while (size > 0) {
        if (ctx->last_len == 16) {
            if (!cmac_block(ctx, ctx->last))
                return 0;
            ctx->last_len = 0;
        }
        size_t take = 16 - (size_t)ctx->last_len;
        if (take > size)
            take = size;
        memcpy(ctx->last + ctx->last_len, data, take);
        ctx->last_len += (int)take;
        data += take;
        size -= take;
    }
    return 1;
}
static int cmac_end(EVP_MAC_CTX *ctx, unsigned char *out) {
    unsigned char subkey[16];
    memset(subkey, 0, sizeof(subkey));
    if (mbedtls_aes_crypt_ecb(&ctx->aes, MBEDTLS_AES_ENCRYPT, subkey, subkey) != 0)
        return 0;
    cmac_double(subkey); /* K1: for a whole last block */
    if (ctx->last_len < 16) {
        cmac_double(subkey); /* K2: for a padded one */
        ctx->last[ctx->last_len] = 0x80;
        memset(ctx->last + ctx->last_len + 1, 0, (size_t)(15 - ctx->last_len));
    }
    for (int i = 0; i < 16; i++)
        ctx->last[i] ^= subkey[i];
    if (!cmac_block(ctx, ctx->last))
        return 0;
    memcpy(out, ctx->chain, 16);
    return 1;
}

EVP_MAC *EVP_MAC_fetch(OSSL_LIB_CTX *lib, const char *name, const char *properties) {
    (void)lib;
    (void)properties;
    if (!name)
        return NULL;
    if (strcmp(name, "HMAC") == 0)
        return (EVP_MAC *)&MAC_OF_HMAC;
    if (strcmp(name, "CMAC") == 0)
        return (EVP_MAC *)&MAC_OF_CMAC;
    return NULL;
}
void EVP_MAC_free(EVP_MAC *mac) { (void)mac; }
EVP_MAC_CTX *EVP_MAC_CTX_new(EVP_MAC *mac) {
    if (!mac)
        return NULL;
    EVP_MAC_CTX *ctx = calloc(1, sizeof(*ctx));
    if (ctx) {
        ctx->kind = mac->kind;
        mbedtls_aes_init(&ctx->aes);
    }
    return ctx;
}
void EVP_MAC_CTX_free(EVP_MAC_CTX *ctx) {
    if (!ctx)
        return;
    md_drop(&ctx->inner);
    mbedtls_aes_free(&ctx->aes);
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}
int EVP_MAC_init(EVP_MAC_CTX *ctx, const unsigned char *key, size_t key_len, const OSSL_PARAM params[]) {
    if (!ctx || !key)
        return 0;
    ctx->ready = 0;
    if (ctx->kind == MAC_HMAC) {
        const EVP_MD *md = ctx->md; /* (no "digest" this time: the one of before) */
        for (const OSSL_PARAM *p = params; p && p->key; p++)
            if (strcmp(p->key, "digest") == 0)
                md = EVP_MD_fetch(NULL, p->text, NULL);
        if (!md || !hmac_start(ctx, md, key, key_len))
            return 0;
    } else {
        if (key_len != 16 && key_len != 24 && key_len != 32)
            return 0;
        if (mbedtls_aes_setkey_enc(&ctx->aes, key, (unsigned)(key_len * 8)) != 0)
            return 0;
        memset(ctx->chain, 0, 16);
        ctx->last_len = 0;
    }
    ctx->ready = 1;
    return 1;
}
int EVP_MAC_update(EVP_MAC_CTX *ctx, const unsigned char *data, size_t size) {
    if (!ctx || !ctx->ready || (size > 0 && !data))
        return 0;
    return ctx->kind == MAC_HMAC ? md_add(&ctx->inner, data, size) : cmac_add(ctx, data, size);
}
int EVP_MAC_final(EVP_MAC_CTX *ctx, unsigned char *out, size_t *out_len, size_t out_size) {
    if (!ctx || !ctx->ready)
        return 0;
    size_t size = ctx->kind == MAC_HMAC ? (size_t)ctx->md->size : 16;
    if (out_len)
        *out_len = size;
    if (!out)
        return 1;
    if (out_size < size)
        return 0;
    ctx->ready = 0;
    return ctx->kind == MAC_HMAC ? hmac_end(ctx, out) : cmac_end(ctx, out);
}

OSSL_PARAM OSSL_PARAM_construct_utf8_string(const char *key, char *text, size_t size) {
    OSSL_PARAM param = { key, text };
    (void)size;
    return param;
}
OSSL_PARAM OSSL_PARAM_construct_end(void) {
    OSSL_PARAM param = { NULL, NULL };
    return param;
}
