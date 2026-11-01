/* TLS backend for wolfSSL, using its native API (no OpenSSL compatibility
 * layer needed). The I/O callbacks move ciphertext through grpc_tls_io.
 * wolfSSL must be built with ALPN support (--enable-alpn). */
#include <limits.h>
#include "internal/grpc_threads.h"
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include "core.h"
#include "internal_tls.h"

#if !defined(HAVE_ALPN)
#error "wolfSSL is built without ALPN, which HTTP/2 requires. Rebuild it with --enable-alpn."
#endif

struct grpc_tls_session {
  WOLFSSL *ssl;
  grpc_tls_io io;
};

static inline WOLFSSL_CTX *wolfssl_ctx(grpc_tls_ctx *ctx) {
  return (WOLFSSL_CTX *)ctx;
}

const char *tls_backend_name(void) {
  return "wolfssl";
}

const char *tls_backend_version(void) {
  return wolfSSL_lib_version();
}

static void wolfssl_init_once(void) {
  (void)wolfSSL_Init();
}

static int io_recv([[maybe_unused]] WOLFSSL *ssl, char *buf, int sz, void *ctx) {
  grpc_tls_session *tls = ctx;
  size_t n = tls_io_pull(&tls->io, buf, (size_t)sz);
  return n == 0 ? WOLFSSL_CBIO_ERR_WANT_READ : (int)n;
}

static int io_send([[maybe_unused]] WOLFSSL *ssl, char *buf, int sz, void *ctx) {
  grpc_tls_session *tls = ctx;
  return tls_io_push(&tls->io, buf, (size_t)sz) ? WOLFSSL_CBIO_ERR_GENERAL : sz;
}

grpc_tls_ctx *tls_backend_ctx_new(const char *cert_file, const char *key_file) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, wolfssl_init_once);

  WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
  if (ctx == nullptr) {
    return nullptr;
  }

  if (wolfSSL_CTX_SetMinVersion(ctx, WOLFSSL_TLSV1_2) != WOLFSSL_SUCCESS ||
      wolfSSL_CTX_use_certificate_chain_file(ctx, cert_file) != WOLFSSL_SUCCESS ||
      wolfSSL_CTX_use_PrivateKey_file(ctx, key_file, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS
#if !defined(NO_CHECK_PRIVATE_KEY)
      || wolfSSL_CTX_check_private_key(ctx) != WOLFSSL_SUCCESS
#endif
  ) {
    wolfSSL_CTX_free(ctx);
    return nullptr;
  }
  wolfSSL_CTX_SetIORecv(ctx, io_recv);
  wolfSSL_CTX_SetIOSend(ctx, io_send);

  return (grpc_tls_ctx *)ctx;
}

void tls_backend_ctx_free(grpc_tls_ctx *ctx) {
  wolfSSL_CTX_free(wolfssl_ctx(ctx));
}

void *tls_backend_ctx_native(grpc_tls_ctx *ctx) {
  return wolfssl_ctx(ctx);
}

grpc_tls_session *tls_backend_session_new(grpc_tls_ctx *ctx) {
  grpc_tls_session *tls = grpc_calloc(1, sizeof(*tls));
  if (tls == nullptr) {
    return nullptr;
  }
  tls->ssl = wolfSSL_new(wolfssl_ctx(ctx));
  if (tls->ssl == nullptr) {
    grpc_free(tls);
    return nullptr;
  }
  wolfSSL_SetIOReadCtx(tls->ssl, tls);
  wolfSSL_SetIOWriteCtx(tls->ssl, tls);

  /* clients that do not offer h2 still connect, as with the other backends */
  char alpn[] = "h2";
  if (wolfSSL_UseALPN(tls->ssl, alpn, sizeof(alpn) - 1, WOLFSSL_ALPN_CONTINUE_ON_MISMATCH) !=
      WOLFSSL_SUCCESS) {
    tls_backend_session_free(tls);
    return nullptr;
  }
  return tls;
}

void tls_backend_session_free(grpc_tls_session *tls) {
  wolfSSL_free(tls->ssl);
  tls_io_free(&tls->io);
  grpc_free(tls);
}

int tls_backend_feed(grpc_tls_session *tls, const void *buf, size_t len) {
  return tls_io_feed(&tls->io, buf, len);
}

int tls_backend_drain(grpc_tls_session *tls, grpc_buf *out) {
  return tls_io_drain(&tls->io, out);
}

int tls_backend_handshake(grpc_tls_session *tls) {
  int ret = wolfSSL_accept(tls->ssl);
  if (ret == WOLFSSL_SUCCESS) {
    return 1;
  }
  int err = wolfSSL_get_error(tls->ssl, ret);
  return err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE ? 0 : -1;
}

ssize_t tls_backend_read(grpc_tls_session *tls, void *buf, size_t len) {
  int ret = wolfSSL_read(tls->ssl, buf, len > INT_MAX ? INT_MAX : (int)len);
  if (ret > 0) {
    return ret;
  }
  switch (wolfSSL_get_error(tls->ssl, ret)) {
  case WOLFSSL_ERROR_WANT_READ:
  case WOLFSSL_ERROR_WANT_WRITE:
    return -2;
  case WOLFSSL_ERROR_ZERO_RETURN:
    return 0;
  default:
    return -1;
  }
}

ssize_t tls_backend_write(grpc_tls_session *tls, const void *buf, size_t len) {
  /* io_send never blocks, so wolfSSL_write() only fails on errors */
  size_t written = 0;
  while (written < len) {
    size_t n = len - written;
    int ret = wolfSSL_write(tls->ssl, (const u8 *)buf + written, n > INT_MAX ? INT_MAX : (int)n);
    if (ret <= 0) {
      return -1;
    }
    written += (size_t)ret;
  }
  return (ssize_t)written;
}

void tls_backend_close_notify(grpc_tls_session *tls) {
  (void)wolfSSL_shutdown(tls->ssl);
}
