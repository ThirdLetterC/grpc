#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grpc.h"
#include "unity.h"
#if defined(GRPC_TLS_OPENSSL) || defined(GRPC_TLS_LIBRESSL) || defined(GRPC_TLS_BORINGSSL) || defined(GRPC_TLS_AWSLC)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include "internal_tls.h"
#endif

void setUp(void) {}
void tearDown(void) {}

static grpc_server *new_tls_server(const char *cert, const char *key) {
  return grpc_server_new(&(grpc_config){
      .port = 19995,
      .ipv4_addr = "127.0.0.1",
      .tls = {.cert_file = cert, .key_file = key},
      .log_level = GRPC_LOG_OFF,
  });
}

void test_tls_backend(void) {
  /* the library must be built with the backend the build asked for */
  TEST_ASSERT_EQUAL_STRING(EXPECTED_TLS_BACKEND, grpc_tls_backend());
  TEST_ASSERT_TRUE(strlen(grpc_tls_backend_version()) > 0);
  printf("TLS backend: %s (%s)\n", grpc_tls_backend(), grpc_tls_backend_version());
}

void test_tls_invalid_files(void) {
  TEST_ASSERT_NULL(new_tls_server("nonexistent_cert.pem", "nonexistent_key.pem"));
  /* a key that does not belong to the certificate */
  TEST_ASSERT_NULL(new_tls_server("test_cert.pem", "test_cert.pem"));
}

void test_tls_server(void) {
  grpc_server *server = new_tls_server("test_cert.pem", "test_key.pem");
  TEST_ASSERT_NOT_NULL(server);
  TEST_ASSERT_NOT_NULL(grpc_server_tls_native(server));
  grpc_server_free(server);

  server = grpc_server_new(
      &(grpc_config){.port = 19995, .ipv4_addr = "127.0.0.1", .log_level = GRPC_LOG_OFF});
  TEST_ASSERT_NOT_NULL(server);
  TEST_ASSERT_NULL(grpc_server_tls_native(server));
  grpc_server_free(server);
}

#if defined(GRPC_TLS_OPENSSL) || defined(GRPC_TLS_LIBRESSL) || defined(GRPC_TLS_BORINGSSL) || defined(GRPC_TLS_AWSLC)
void test_tls_error_isolation(void) {
  grpc_tls_ctx *ctx = tls_backend_ctx_new("test_cert.pem", "test_key.pem");
  TEST_ASSERT_NOT_NULL(ctx);
  grpc_tls_session *session = tls_backend_session_new(ctx);
  TEST_ASSERT_NOT_NULL(session);
  /* A failed connection can leave errors on this same event-loop thread. */
  ERR_put_error(ERR_LIB_SSL, 0, ERR_R_INTERNAL_ERROR, __FILE__, __LINE__);
  TEST_ASSERT_EQUAL_INT(0, tls_backend_handshake(session));
  ERR_put_error(ERR_LIB_SSL, 0, ERR_R_INTERNAL_ERROR, __FILE__, __LINE__);
  char byte;
  TEST_ASSERT_EQUAL_INT(-2, tls_backend_read(session, &byte, 1));
  tls_backend_session_free(session);
  tls_backend_ctx_free(ctx);
}
#endif

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_tls_backend);
  RUN_TEST(test_tls_invalid_files);
  RUN_TEST(test_tls_server);
#if defined(GRPC_TLS_OPENSSL) || defined(GRPC_TLS_LIBRESSL) || defined(GRPC_TLS_BORINGSSL) || defined(GRPC_TLS_AWSLC)
  RUN_TEST(test_tls_error_isolation);
#endif
  return UNITY_END();
}
