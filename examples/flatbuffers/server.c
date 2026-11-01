/* A gRPC server whose messages are FlatBuffers, built and read with flatcc.
 *
 *   ./flatbuffers_server.bin [port] [quiet]
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grpc.h"
#include "greeter_builder.h"
#include "greeter_verifier.h"

enum { MAX_REPLIES = 100 };

static grpc_server *g_server;
static int g_quiet = 0;

static void sig_handler([[maybe_unused]] int sig) {
  grpc_server_stop(g_server);
}

/* builds every response. grpc_call_send_fb() resets it after each one */
static flatcc_builder_t g_builder;

static void reply(grpc_call *call, const char *text, int32_t index) {
  greeter_HelloReply_start_as_root(&g_builder);
  greeter_HelloReply_message_create_str(&g_builder, text);
  greeter_HelloReply_index_add(&g_builder, index);
  greeter_HelloReply_end_as_root(&g_builder);
  grpc_call_send_fb(call, &g_builder);
}

static const char *request_name(greeter_HelloRequest_table_t req) {
  const char *name = greeter_HelloRequest_name(req);
  return name != nullptr && name[0] != '\0' ? name : "World";
}

/* verifies the request; the table points into it and lives as long as the call */
static greeter_HelloRequest_table_t read_request(grpc_call *call,
                                                 const grpc_message *request) {
  greeter_HelloRequest_table_t req = GRPC_FB_UNPACK(greeter_HelloRequest, request);
  if (req == nullptr)
    grpc_call_finish(call, GRPC_STATUS_INVALID_ARGUMENT,
                        "request is not a valid greeter.HelloRequest");
  return req;
}

/* SayHello (unary) */
static void say_hello(grpc_call *call, const grpc_message *request,
                      [[maybe_unused]] void *userdata) {
  greeter_HelloRequest_table_t req = read_request(call, request);
  if (req == nullptr)
    return;
  if (!g_quiet)
    printf("[server] SayHello name='%s'\n", request_name(req));

  char text[256];
  snprintf(text, sizeof(text), "Hello, %s!", request_name(req));
  reply(call, text, 0);
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

/* SayManyHellos (server streaming) */
static void say_many_hellos(grpc_call *call, const grpc_message *request,
                            [[maybe_unused]] void *userdata) {
  greeter_HelloRequest_table_t req = read_request(call, request);
  if (req == nullptr)
    return;
  int32_t count = greeter_HelloRequest_count(req);
  if (count < 0 || count > MAX_REPLIES) {
    grpc_call_finish(call, GRPC_STATUS_INVALID_ARGUMENT, "count must be between 0 and 100");
    return;
  }
  if (!g_quiet)
    printf("[server] SayManyHellos name='%s' count=%d\n", request_name(req), count);

  for (int32_t i = 0; i < count; i++) {
    char text[256];
    snprintf(text, sizeof(text), "Hello #%d, %s!", i, request_name(req));
    reply(call, text, i);
  }
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

int main(int argc, char *argv[]) {
  int port = argc > 1 ? atoi(argv[1]) : 19029;
  g_quiet = argc > 2 ? atoi(argv[2]) : 0;
  setvbuf(stdout, nullptr, _IONBF, 0);

  if (grpc_fb_builder_init(&g_builder) != GRPC_OK)
    return 1;

  g_server = grpc_server_new(&(grpc_config){
      .port = (uint16_t)port,
      .log_level = g_quiet ? GRPC_LOG_WARNING : GRPC_LOG_INFO,
  });
  if (g_server == nullptr) {
    fprintf(stderr, "Failed to create server on port %d\n", port);
    return 1;
  }

  grpc_server_handle(g_server, "/greeter.Greeter/SayHello", say_hello, nullptr);
  grpc_server_handle(g_server, "/greeter.Greeter/SayManyHellos", say_many_hellos, nullptr);
  printf("[server] FlatBuffers Greeter listening on port %d\n", port);

#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  grpc_server_run(g_server);

  printf("[server] Shutting down...\n");
  grpc_server_free(g_server);
  flatcc_builder_clear(&g_builder);
  return 0;
}
