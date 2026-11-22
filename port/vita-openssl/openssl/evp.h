/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
 *
 * The part of OpenSSL's EVP interface Vita3K is written with (packages, firmware and game
 * decryption: AES-ECB/CBC/CTR without padding, SHA-1, SHA-256, HMAC, CMAC), over Mbed TLS
 * (port/ps4_vita_evp.c). EmuSuNi's own; only the names and the call shapes are OpenSSL's.
 *
 * What it doesn't do: padding (every caller turns it off; with padding on, the Final of a block
 * cipher fails), engines, properties, any other algorithm.
 */
#ifndef EMUSUNI_VITA_OPENSSL_EVP_H
#define EMUSUNI_VITA_OPENSSL_EVP_H

#include <stddef.h>
#include <stdint.h> /* (OpenSSL's brings it, and psvpfstools counts on that) */

/* (Own names in the object: another library in the same link may bring OpenSSL's.) */
#define EVP_CIPHER_fetch emusuni_EVP_CIPHER_fetch
#define EVP_CIPHER_free emusuni_EVP_CIPHER_free
#define EVP_aes_128_ecb emusuni_EVP_aes_128_ecb
#define EVP_aes_128_cbc emusuni_EVP_aes_128_cbc
#define EVP_aes_128_ctr emusuni_EVP_aes_128_ctr
#define EVP_CIPHER_CTX_new emusuni_EVP_CIPHER_CTX_new
#define EVP_CIPHER_CTX_free emusuni_EVP_CIPHER_CTX_free
#define EVP_CIPHER_CTX_set_padding emusuni_EVP_CIPHER_CTX_set_padding
#define EVP_EncryptInit_ex emusuni_EVP_EncryptInit_ex
#define EVP_EncryptUpdate emusuni_EVP_EncryptUpdate
#define EVP_EncryptFinal_ex emusuni_EVP_EncryptFinal_ex
#define EVP_DecryptInit_ex emusuni_EVP_DecryptInit_ex
#define EVP_DecryptUpdate emusuni_EVP_DecryptUpdate
#define EVP_DecryptFinal_ex emusuni_EVP_DecryptFinal_ex
#define EVP_MD_fetch emusuni_EVP_MD_fetch
#define EVP_MD_free emusuni_EVP_MD_free
#define EVP_sha1 emusuni_EVP_sha1
#define EVP_sha256 emusuni_EVP_sha256
#define EVP_MD_CTX_new emusuni_EVP_MD_CTX_new
#define EVP_MD_CTX_free emusuni_EVP_MD_CTX_free
#define EVP_DigestInit_ex emusuni_EVP_DigestInit_ex
#define EVP_DigestUpdate emusuni_EVP_DigestUpdate
#define EVP_DigestFinal_ex emusuni_EVP_DigestFinal_ex
#define EVP_DigestFinal emusuni_EVP_DigestFinal
#define EVP_Digest emusuni_EVP_Digest
#define HMAC emusuni_HMAC
#define EVP_MAC_fetch emusuni_EVP_MAC_fetch
#define EVP_MAC_free emusuni_EVP_MAC_free
#define EVP_MAC_CTX_new emusuni_EVP_MAC_CTX_new
#define EVP_MAC_CTX_free emusuni_EVP_MAC_CTX_free
#define EVP_MAC_init emusuni_EVP_MAC_init
#define EVP_MAC_update emusuni_EVP_MAC_update
#define EVP_MAC_final emusuni_EVP_MAC_final
#define OSSL_PARAM_construct_utf8_string emusuni_OSSL_PARAM_construct_utf8_string
#define OSSL_PARAM_construct_end emusuni_OSSL_PARAM_construct_end

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_mac_st EVP_MAC;
typedef struct evp_mac_ctx_st EVP_MAC_CTX;
typedef struct emusuni_evp_engine ENGINE;
typedef struct emusuni_evp_lib_ctx OSSL_LIB_CTX;
typedef struct ossl_param_st {
    const char *key; /* NULL: the end of the list */
    const char *text;
} OSSL_PARAM;

#define EVP_MAX_MD_SIZE 64

/* Every function that returns int: 1 done, 0 failed. */

/* "AES-<128|192|256>-<ECB|CBC|CTR>"; NULL for anything else. Nothing to free. */
EVP_CIPHER *EVP_CIPHER_fetch(OSSL_LIB_CTX *lib, const char *name, const char *properties);
void EVP_CIPHER_free(EVP_CIPHER *cipher);
const EVP_CIPHER *EVP_aes_128_ecb(void);
const EVP_CIPHER *EVP_aes_128_cbc(void);
const EVP_CIPHER *EVP_aes_128_ctr(void);

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);
int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int padding);
/* cipher NULL: the one of the Init before; key NULL: the key of before; iv NULL: zeros (ECB has none). */
int EVP_EncryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *engine,
                       const unsigned char *key, const unsigned char *iv);
int EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, ENGINE *engine,
                       const unsigned char *key, const unsigned char *iv);
/* ECB and CBC write whole blocks and keep the rest for the next call; in place (out == in) only
 * while every call brings whole blocks. CTR writes everything, in place or not. */
int EVP_EncryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len, const unsigned char *in, int in_len);
int EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len, const unsigned char *in, int in_len);
/* Writes nothing. Fails for ECB and CBC with part of a block left, or with padding on. */
int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len);
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len);

/* "SHA1" or "SHA256" (also "SHA-1", "SHA-256", "SHA2-256"). Nothing to free. */
EVP_MD *EVP_MD_fetch(OSSL_LIB_CTX *lib, const char *name, const char *properties);
void EVP_MD_free(EVP_MD *md);
const EVP_MD *EVP_sha1(void);
const EVP_MD *EVP_sha256(void);

EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *ctx);
int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *md, ENGINE *engine);
int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *data, size_t size);
int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *out, unsigned int *out_len);
int EVP_DigestFinal(EVP_MD_CTX *ctx, unsigned char *out, unsigned int *out_len);
int EVP_Digest(const void *data, size_t size, unsigned char *out, unsigned int *out_len, const EVP_MD *md,
               ENGINE *engine);

/* out, or NULL when it failed. */
unsigned char *HMAC(const EVP_MD *md, const void *key, int key_len, const unsigned char *data, size_t size,
                    unsigned char *out, unsigned int *out_len);

/* "HMAC" (its parameter "digest": "SHA1" or "SHA256") or "CMAC" (AES; the key of 16, 24 or 32 bytes
 * says which, its parameter "cipher" isn't read). Nothing to free but the contexts. */
EVP_MAC *EVP_MAC_fetch(OSSL_LIB_CTX *lib, const char *name, const char *properties);
void EVP_MAC_free(EVP_MAC *mac);
EVP_MAC_CTX *EVP_MAC_CTX_new(EVP_MAC *mac);
void EVP_MAC_CTX_free(EVP_MAC_CTX *ctx);
int EVP_MAC_init(EVP_MAC_CTX *ctx, const unsigned char *key, size_t key_len, const OSSL_PARAM params[]);
int EVP_MAC_update(EVP_MAC_CTX *ctx, const unsigned char *data, size_t size);
/* out NULL: only the size in *out_len. Fails when out_size is less than the MAC's size. */
int EVP_MAC_final(EVP_MAC_CTX *ctx, unsigned char *out, size_t *out_len, size_t out_size);

OSSL_PARAM OSSL_PARAM_construct_utf8_string(const char *key, char *text, size_t size);
OSSL_PARAM OSSL_PARAM_construct_end(void);

#ifdef __cplusplus
}
#endif

#endif
