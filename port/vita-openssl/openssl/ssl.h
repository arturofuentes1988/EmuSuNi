/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
 *
 * The Vita's TLS, off: the names Vita3K's SceSsl, SceHttp and http are written with, each one
 * answering that there is no TLS (no context, no connection). A Vita game that asks for an https
 * page gets a connection error, as with the network cable out. EmuSuNi's own.
 */
#ifndef EMUSUNI_VITA_OPENSSL_SSL_H
#define EMUSUNI_VITA_OPENSSL_SSL_H

typedef struct emusuni_ssl SSL;
typedef struct emusuni_ssl_ctx SSL_CTX;
typedef struct emusuni_ssl_method SSL_METHOD;

#define SSL_ERROR_SSL 1
#define SSL_MODE_AUTO_RETRY 4L
#define X509_V_OK 0
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY 20

static inline int SSL_library_init(void) { return 1; }
static inline void SSL_load_error_strings(void) {}
static inline const SSL_METHOD *TLS_method(void) { return 0; }
static inline SSL_CTX *SSL_CTX_new(const SSL_METHOD *method) { (void)method; return 0; }
static inline void SSL_CTX_free(SSL_CTX *ctx) { (void)ctx; }
static inline long SSL_CTX_set_mode(SSL_CTX *ctx, long mode) { (void)ctx; (void)mode; return 0; }
static inline unsigned long long SSL_CTX_set_options(SSL_CTX *ctx, unsigned long long options) { (void)ctx; (void)options; return 0; }
static inline unsigned long long SSL_CTX_clear_options(SSL_CTX *ctx, unsigned long long options) { (void)ctx; (void)options; return 0; }
static inline SSL *SSL_new(SSL_CTX *ctx) { (void)ctx; return 0; }
static inline void SSL_free(SSL *ssl) { (void)ssl; }
static inline int SSL_set_fd(SSL *ssl, int fd) { (void)ssl; (void)fd; return 0; }
static inline void SSL_set_security_level(SSL *ssl, int level) { (void)ssl; (void)level; }
static inline int SSL_connect(SSL *ssl) { (void)ssl; return -1; }
static inline int SSL_get_error(const SSL *ssl, int result) { (void)ssl; (void)result; return SSL_ERROR_SSL; }
static inline long SSL_get_verify_result(const SSL *ssl) { (void)ssl; return 1; }
static inline int SSL_write(SSL *ssl, const void *data, int size) { (void)ssl; (void)data; (void)size; return -1; }
static inline int SSL_read(SSL *ssl, void *data, int size) { (void)ssl; (void)data; (void)size; return -1; }
static inline SSL_CTX *SSL_get_SSL_CTX(const SSL *ssl) { (void)ssl; return 0; }

#endif
