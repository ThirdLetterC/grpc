/* TLS backend for rustls, through its C API (rustls-ffi 0.15). Ciphertext is
 * moved with rustls_connection_read_tls() and rustls_connection_write_tls()
 * callbacks. rustls only speaks TLS 1.2 and 1.3. */
#include <errno.h>
#include "internal/grpc_threads.h"
#include <stdio.h>
#include <rustls.h>
#include "core.h"
#include "internal_tls.h"

struct grpc_tls_session {
  struct rustls_connection *conn;
  grpc_tls_io io;
};

static inline const struct rustls_server_config *rustls_config(grpc_tls_ctx *ctx) {
  return (const struct rustls_server_config *)ctx;
}

const char *tls_backend_name(void) {
  return "rustls";
}

/* rustls_version() is not NUL-terminated. */
static char version[128];

static void version_init(void) {
  struct rustls_str v = rustls_version();
  snprintf(version, sizeof(version), "%.*s", (int)v.len, v.data);
}

const char *tls_backend_version(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, version_init);
  return version;
}

static int read_file(const char *path, grpc_buf *out) {
  FILE *fp = fopen(path, "rb");
  if (fp == nullptr)
    return 1;
  int res = 0;
  for (;;) {
    if (buf_reserve(out, 4096)) {
      res = 1;
      break;
    }
    size_t n = fread(out->data + out->len, 1, out->cap - out->len, fp);
    out->len += n;
    if (n == 0) {
      res = ferror(fp) ? 1 : 0;
      break;
    }
  }
  fclose(fp);
  return res;
}

grpc_tls_ctx *tls_backend_ctx_new(const char *cert_file, const char *key_file) {
  grpc_buf cert = {}, key = {};
  const struct rustls_certified_key *certified_key = nullptr;
  const struct rustls_server_config *config = nullptr;

  if (read_file(cert_file, &cert) || read_file(key_file, &key))
    goto out;
  /* also checks that the key belongs to the certificate */
  if (rustls_certified_key_build(cert.data, cert.len, key.data, key.len, &certified_key) !=
      RUSTLS_RESULT_OK)
    goto out;

  struct rustls_server_config_builder *builder = rustls_server_config_builder_new();
  if (builder == nullptr)
    goto out;
  /* rustls rejects clients that offer ALPN without h2. clients that offer
   * no ALPN at all still connect */
  struct rustls_slice_bytes alpn = {.data = (const uint8_t *)"h2", .len = 2};
  if (rustls_server_config_builder_set_alpn_protocols(builder, &alpn, 1) != RUSTLS_RESULT_OK ||
      rustls_server_config_builder_set_certified_keys(builder, &certified_key, 1) !=
          RUSTLS_RESULT_OK) {
    rustls_server_config_builder_free(builder);
    goto out;
  }
  /* consumes the builder, even on failure */
  if (rustls_server_config_builder_build(builder, &config) != RUSTLS_RESULT_OK)
    config = nullptr;

out:
  /* the config keeps its own reference to the key */
  rustls_certified_key_free(certified_key);
  buf_free(&cert);
  /* do not leave the private key behind in freed memory */
  if (key.data != nullptr)
    memset(key.data, 0, key.cap);
  buf_free(&key);
  return (grpc_tls_ctx *)config;
}

void tls_backend_ctx_free(grpc_tls_ctx *ctx) {
  rustls_server_config_free(rustls_config(ctx));
}

void *tls_backend_ctx_native(grpc_tls_ctx *ctx) {
  return (void *)rustls_config(ctx);
}

grpc_tls_session *tls_backend_session_new(grpc_tls_ctx *ctx) {
  grpc_tls_session *tls = grpc_calloc(1, sizeof(*tls));
  if (tls == nullptr) {
    return nullptr;
  }
  if (rustls_server_connection_new(rustls_config(ctx), &tls->conn) != RUSTLS_RESULT_OK) {
    grpc_free(tls);
    return nullptr;
  }
  return tls;
}

void tls_backend_session_free(grpc_tls_session *tls) {
  rustls_connection_free(tls->conn);
  tls_io_free(&tls->io);
  grpc_free(tls);
}

int tls_backend_feed(grpc_tls_session *tls, const void *buf, size_t len) {
  return tls_io_feed(&tls->io, buf, len);
}

static rustls_io_result io_read(void *userdata, uint8_t *buf, size_t n, size_t *out_n) {
  grpc_tls_session *tls = userdata;
  *out_n = tls_io_pull(&tls->io, buf, n);
  /* returning 0 bytes would mean EOF to rustls */
  return *out_n == 0 ? EAGAIN : 0;
}

static rustls_io_result io_write(void *userdata, const uint8_t *buf, size_t n, size_t *out_n) {
  grpc_tls_session *tls = userdata;
  if (tls_io_push(&tls->io, buf, n))
    return ENOMEM;
  *out_n = n;
  return 0;
}

/* moves the ciphertext rustls has ready into io.out */
static int flush_tls(grpc_tls_session *tls) {
  while (rustls_connection_wants_write(tls->conn)) {
    size_t n = 0;
    if (rustls_connection_write_tls(tls->conn, io_write, tls, &n) != 0)
      return 1;
  }
  return 0;
}

int tls_backend_drain(grpc_tls_session *tls, grpc_buf *out) {
  if (flush_tls(tls))
    return 1;
  return tls_io_drain(&tls->io, out);
}

/* hands one batch of buffered ciphertext to rustls and decrypts it.
 * returns 1 when something was consumed, 0 when nothing was, -1 on error */
static int pump(grpc_tls_session *tls) {
  if (tls_io_pending(&tls->io) == 0)
    return 0;
  size_t n = 0;
  rustls_io_result res = rustls_connection_read_tls(tls->conn, io_read, tls, &n);
  if (res != 0 && res != EAGAIN)
    return -1;
  /* also on errors, so that an alert reaches the peer */
  if (rustls_connection_process_new_packets(tls->conn) != RUSTLS_RESULT_OK)
    return -1;
  return n > 0 ? 1 : 0;
}

int tls_backend_handshake(grpc_tls_session *tls) {
  /* stop once the handshake is done: what follows is application data
   * and is decrypted by tls_backend_read() as it is consumed */
  while (rustls_connection_is_handshaking(tls->conn)) {
    int res = pump(tls);
    if (res < 0)
      return -1;
    if (res == 0)
      return 0;
  }
  return 1;
}

ssize_t tls_backend_read(grpc_tls_session *tls, void *buf, size_t len) {
  for (;;) {
    size_t n = 0;
    rustls_result res = rustls_connection_read(tls->conn, buf, len, &n);
    if (res == RUSTLS_RESULT_OK) {
      /* 0 is the peer's close_notify */
      return (ssize_t)n;
    }
    if (res != RUSTLS_RESULT_PLAINTEXT_EMPTY)
      return -1;

    int pumped = pump(tls);
    if (pumped < 0)
      return -1;
    if (pumped == 0)
      return -2;
  }
}

ssize_t tls_backend_write(grpc_tls_session *tls, const void *buf, size_t len) {
  size_t written = 0;
  while (written < len) {
    size_t n = 0;
    if (rustls_connection_write(tls->conn, (const u8 *)buf + written, len - written, &n) !=
        RUSTLS_RESULT_OK)
      return -1;
    if (n == 0) {
      /* rustls buffers a limited amount of output. take it and retry */
      if (!rustls_connection_wants_write(tls->conn) || flush_tls(tls))
        return -1;
      continue;
    }
    written += n;
  }
  return (ssize_t)written;
}

void tls_backend_close_notify(grpc_tls_session *tls) {
  rustls_connection_send_close_notify(tls->conn);
}
