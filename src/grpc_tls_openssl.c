/* TLS backend for the OpenSSL API: OpenSSL, LibreSSL, BoringSSL and AWS-LC.
 * The connection runs over memory BIOs; the event loop moves the ciphertext. */
#include <limits.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include "core.h"
#include "internal_tls.h"

/* make sure the headers in the include path belong to the requested library */
#if defined(GRPC_TLS_AWSLC)
#if !defined(OPENSSL_IS_AWSLC)
#error "TLS=awslc, but the <openssl/ssl.h> found is not AWS-LC. Set TLS_PREFIX."
#endif
static const char BACKEND_NAME[] = "awslc";
#elif defined(GRPC_TLS_BORINGSSL)
#if !defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
#error "TLS=boringssl, but the <openssl/ssl.h> found is not BoringSSL. Set TLS_PREFIX."
#endif
static const char BACKEND_NAME[] = "boringssl";
#elif defined(GRPC_TLS_LIBRESSL)
#if !defined(LIBRESSL_VERSION_NUMBER)
#error "TLS=libressl, but the <openssl/ssl.h> found is not LibreSSL. Set TLS_PREFIX."
#endif
static const char BACKEND_NAME[] = "libressl";
#elif defined(GRPC_TLS_OPENSSL)
#if defined(LIBRESSL_VERSION_NUMBER) || defined(OPENSSL_IS_BORINGSSL)
#error "TLS=openssl, but the <openssl/ssl.h> found is not OpenSSL."
#endif
static const char BACKEND_NAME[] = "openssl";
#else
#error "grpc_tls_openssl.c needs one of GRPC_TLS_{OPENSSL,LIBRESSL,BORINGSSL,AWSLC}"
#endif

/* the library objects are used directly as the opaque grpc types */
static inline SSL_CTX *ssl_ctx(grpc_tls_ctx *ctx) {
  return (SSL_CTX *)ctx;
}

static inline SSL *ssl(grpc_tls_session *tls) {
  return (SSL *)tls;
}

const char *tls_backend_name(void) {
  return BACKEND_NAME;
}

const char *tls_backend_version(void) {
  return OpenSSL_version(OPENSSL_VERSION);
}

/* picks "h2" from the client's list. done by hand because
 * SSL_select_next_proto() differs between the OpenSSL forks */
static int alpn_select_cb([[maybe_unused]] SSL *s, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, [[maybe_unused]] void *arg) {
  for (unsigned int i = 0; i < inlen;) {
    unsigned int len = in[i];
    if (i + 1 + len > inlen)
      break;
    if (len == 2 && memcmp(in + i + 1, "h2", 2) == 0) {
      *out = in + i + 1;
      *outlen = 2;
      return SSL_TLSEXT_ERR_OK;
    }
    i += 1 + len;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

grpc_tls_ctx *tls_backend_ctx_new(const char *cert_file, const char *key_file) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  if (ctx == nullptr) {
    return nullptr;
  }

  if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
      SSL_CTX_use_certificate_chain_file(ctx, cert_file) <= 0 ||
      SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0 ||
      !SSL_CTX_check_private_key(ctx)) {
    SSL_CTX_free(ctx);
    return nullptr;
  }
  SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, nullptr);

  return (grpc_tls_ctx *)ctx;
}

void tls_backend_ctx_free(grpc_tls_ctx *ctx) {
  SSL_CTX_free(ssl_ctx(ctx));
}

void *tls_backend_ctx_native(grpc_tls_ctx *ctx) {
  return ssl_ctx(ctx);
}

grpc_tls_session *tls_backend_session_new(grpc_tls_ctx *ctx) {
  SSL *s = SSL_new(ssl_ctx(ctx));
  if (s == nullptr) {
    return nullptr;
  }

  BIO *rbio = BIO_new(BIO_s_mem());
  BIO *wbio = BIO_new(BIO_s_mem());
  if (rbio == nullptr || wbio == nullptr) {
    BIO_free(rbio);
    BIO_free(wbio);
    SSL_free(s);
    return nullptr;
  }
  /* an empty read BIO means "wait for more data", not EOF */
  BIO_set_mem_eof_return(rbio, -1);
  SSL_set_bio(s, rbio, wbio);
  SSL_set_accept_state(s);
  return (grpc_tls_session *)s;
}

void tls_backend_session_free(grpc_tls_session *tls) {
  SSL_free(ssl(tls));
}

int tls_backend_feed(grpc_tls_session *tls, const void *buf, size_t len) {
  BIO *rbio = SSL_get_rbio(ssl(tls));
  while (len > 0) {
    int n = BIO_write(rbio, buf, len > INT_MAX ? INT_MAX : (int)len);
    if (n <= 0) {
      return 1;
    }
    buf = (const u8 *)buf + n;
    len -= (size_t)n;
  }
  return 0;
}

int tls_backend_drain(grpc_tls_session *tls, grpc_buf *out) {
  BIO *wbio = SSL_get_wbio(ssl(tls));
  size_t pending;
  while ((pending = BIO_ctrl_pending(wbio)) > 0) {
    if (buf_reserve(out, pending)) {
      return 1;
    }
    int n = BIO_read(wbio, out->data + out->len, pending > INT_MAX ? INT_MAX : (int)pending);
    if (n <= 0) {
      return 1;
    }
    out->len += (size_t)n;
  }
  return 0;
}

int tls_backend_handshake(grpc_tls_session *tls) {
  ERR_clear_error();
  int ret = SSL_do_handshake(ssl(tls));
  if (ret == 1) {
    return 1;
  }
  int err = SSL_get_error(ssl(tls), ret);
  return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ? 0 : -1;
}

ssize_t tls_backend_read(grpc_tls_session *tls, void *buf, size_t len) {
  ERR_clear_error();
  int ret = SSL_read(ssl(tls), buf, len > INT_MAX ? INT_MAX : (int)len);
  if (ret > 0) {
    return ret;
  }
  switch (SSL_get_error(ssl(tls), ret)) {
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
    return -2;
  case SSL_ERROR_ZERO_RETURN:
    return 0;
  default:
    return -1;
  }
}

ssize_t tls_backend_write(grpc_tls_session *tls, const void *buf, size_t len) {
  /* a memory BIO accepts everything, so SSL_write() only fails on errors */
  size_t written = 0;
  while (written < len) {
    size_t n = len - written;
    ERR_clear_error();
    int ret = SSL_write(ssl(tls), (const u8 *)buf + written, n > INT_MAX ? INT_MAX : (int)n);
    if (ret <= 0) {
      return -1;
    }
    written += (size_t)ret;
  }
  return (ssize_t)written;
}

void tls_backend_close_notify(grpc_tls_session *tls) {
  ERR_clear_error();
  (void)SSL_shutdown(ssl(tls));
}
