/* The Greeter: every kind of gRPC method, served with handlers.
 *
 *   ./grpc_server.bin [port] [quiet] [tls] [cert] [key]
 *   THREADS=N ./grpc_server.bin   one server (event loop) per thread on the same port
 */
#include "internal/grpc_threads.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "grpc.h"
#include "helloworld.pb-c.h"

enum { MAX_SERVERS = 256 };

static grpc_server *g_servers[MAX_SERVERS];
static int g_nb_servers;
static int g_quiet;

static void on_signal([[maybe_unused]] int sig) {
  /* grpc_server_stop() is async-signal-safe */
  for (int i = 0; i < g_nb_servers; i++)
    grpc_server_stop(g_servers[i]);
}

static const char *request_name(const Helloworld__HelloRequest *req, const char *fallback) {
  return req->name != nullptr && req->name[0] != '\0' ? req->name : fallback;
}

static Helloworld__HelloRequest *unpack_request(grpc_call *call,
                                                const grpc_message *message) {
  Helloworld__HelloRequest *req = GRPC_PB_UNPACK(
      Helloworld__HelloRequest, &helloworld__hello_request__descriptor, message);
  if (req == nullptr)
    grpc_call_finish(call, GRPC_STATUS_INVALID_ARGUMENT, "invalid HelloRequest");
  return req;
}

static void send_reply(grpc_call *call, const char *text) {
  Helloworld__HelloReply reply = HELLOWORLD__HELLO_REPLY__INIT;
  reply.message = (char *)text;
  grpc_call_send_pb(call, &reply.base);
}

/* 1. SayHello (unary), with a trailer */
static void say_hello(grpc_call *call, const grpc_message *request,
                      [[maybe_unused]] void *userdata) {
  Helloworld__HelloRequest *req = unpack_request(call, request);
  if (req == nullptr)
    return;
  if (!g_quiet)
    printf("[server] SayHello name='%s'\n", request_name(req, "World"));

  char text[256];
  snprintf(text, sizeof(text), "Hello, %s!", request_name(req, "World"));
  send_reply(call, text);
  GRPC_PB_FREE_UNPACKED(req);

  grpc_call_add_trailers(call, &(grpc_metadata){.key = "x-custom-reply", .value = "pong"},
                            1);
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

/* 2. SayHelloAgain (unary) */
static void say_hello_again(grpc_call *call, const grpc_message *request,
                            [[maybe_unused]] void *userdata) {
  Helloworld__HelloRequest *req = unpack_request(call, request);
  if (req == nullptr)
    return;
  char text[256];
  snprintf(text, sizeof(text), "Hello again, %s!", request_name(req, "World"));
  send_reply(call, text);
  GRPC_PB_FREE_UNPACKED(req);
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

/* 3. SayHelloStream (server streaming: one request, three replies) */
static void say_hello_stream(grpc_call *call, const grpc_message *request,
                             [[maybe_unused]] void *userdata) {
  Helloworld__HelloRequest *req = unpack_request(call, request);
  if (req == nullptr)
    return;
  for (int i = 1; i <= 3; i++) {
    char text[256];
    snprintf(text, sizeof(text), "Stream message #%d for %s", i, request_name(req, "World"));
    send_reply(call, text);
  }
  GRPC_PB_FREE_UNPACKED(req);
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

/* 4. SayHelloClientStream (client streaming: many requests, one reply) */
typedef struct names {
  size_t count;
  char text[512];
} names;

static void client_stream_on_message(grpc_call *call, const grpc_message *message,
                                     [[maybe_unused]] void *userdata) {
  names *state = grpc_call_userdata(call);
  if (state == nullptr) {
    state = calloc(1, sizeof(*state));
    if (state == nullptr) {
      grpc_call_finish(call, GRPC_STATUS_RESOURCE_EXHAUSTED, nullptr);
      return;
    }
    grpc_call_set_userdata(call, state, free);
  }
  Helloworld__HelloRequest *req = unpack_request(call, message);
  if (req == nullptr)
    return;
  size_t used = strlen(state->text);
  snprintf(state->text + used, sizeof(state->text) - used, "%s%s", state->count ? ", " : "",
           request_name(req, "Friend"));
  state->count++;
  GRPC_PB_FREE_UNPACKED(req);
}

static void client_stream_on_end(grpc_call *call, [[maybe_unused]] void *userdata) {
  names *state = grpc_call_userdata(call);
  char text[640];
  snprintf(text, sizeof(text), "Hello to all: %s (Total: %zu)", state ? state->text : "",
           state ? state->count : 0);
  if (!g_quiet)
    printf("[server] SayHelloClientStream: %s\n", text);
  send_reply(call, text);
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

/* 5. SayHelloBidiStream (one reply per request) */
static void bidi_on_message(grpc_call *call, const grpc_message *message,
                            [[maybe_unused]] void *userdata) {
  Helloworld__HelloRequest *req = unpack_request(call, message);
  if (req == nullptr)
    return;
  char text[256];
  snprintf(text, sizeof(text), "Bidi reply to: %s", request_name(req, "World"));
  send_reply(call, text);
  GRPC_PB_FREE_UNPACKED(req);
}

static void bidi_on_end(grpc_call *call, [[maybe_unused]] void *userdata) {
  grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
}

/* 6. SayHelloWithError (rich error model) */
static void say_hello_with_error(grpc_call *call,
                                 [[maybe_unused]] const grpc_message *request,
                                 [[maybe_unused]] void *userdata) {
  /* google.rpc.ErrorInfo { reason: "INVALID_NAME", domain: "helloworld" } */
  static const uint8_t error_info[] = {
      0x0a, 0x0c, 'I', 'N', 'V', 'A', 'L', 'I', 'D', '_', 'N', 'A', 'M',
      'E',  0x12, 0x0a, 'h', 'e', 'l', 'l', 'o', 'w', 'o', 'r', 'l', 'd',
  };
  grpc_status_detail_entry detail = {
      .type_url = "type.googleapis.com/google.rpc.ErrorInfo",
      .value = error_info,
      .value_len = sizeof(error_info),
  };
  grpc_call_finish_with_details(call, GRPC_STATUS_INVALID_ARGUMENT,
                                   "Name validation failed: name cannot be empty", &detail, 1);
}

/* 7. SayHelloSlow: answered by a worker thread */
static void *slow_worker(void *arg) {
  grpc_call *call = arg;
  uv_sleep(300);

  Helloworld__HelloRequest *req = GRPC_PB_UNPACK(
      Helloworld__HelloRequest, &helloworld__hello_request__descriptor, grpc_call_request(call));
  char text[256];
  snprintf(text, sizeof(text), "Hello slowly, %s!", req ? request_name(req, "World") : "World");
  GRPC_PB_FREE_UNPACKED(req);

  Helloworld__HelloReply reply = HELLOWORLD__HELLO_REPLY__INIT;
  reply.message = text;
  /* if the client's deadline passed meanwhile, these fail harmlessly */
  grpc_call_send_async(call, grpc_pb_pack(&reply.base));
  grpc_call_finish_async(call, GRPC_STATUS_OK, nullptr);
  grpc_call_unref(call);
  return nullptr;
}

static void say_hello_slow(grpc_call *call, [[maybe_unused]] const grpc_message *request,
                           [[maybe_unused]] void *userdata) {
  pthread_t thread;
  grpc_call_ref(call);
  if (pthread_create(&thread, nullptr, slow_worker, call) != 0) {
    grpc_call_unref(call);
    grpc_call_finish(call, GRPC_STATUS_RESOURCE_EXHAUSTED, nullptr);
    return;
  }
  pthread_detach(thread);
}

/* an interceptor that logs every call, and refuses those asking for it */
static grpc_status log_call(grpc_call *call, const char **message,
                               [[maybe_unused]] void *userdata) {
  if (!g_quiet)
    printf("[interceptor] %s from %s\n", grpc_call_method(call), grpc_call_peer(call));
  const char *reject = grpc_call_metadata(call, "x-reject", nullptr);
  if (reject != nullptr && strcmp(reject, "yes") == 0) {
    *message = "rejected by the interceptor";
    return GRPC_STATUS_PERMISSION_DENIED;
  }
  return GRPC_STATUS_OK;
}

static void log_done(grpc_call *call, grpc_status status, uint64_t duration_ms,
                     [[maybe_unused]] void *userdata) {
  if (!g_quiet)
    printf("[interceptor] %s done: %s in %llu ms\n", grpc_call_method(call),
           grpc_status_str(status), (unsigned long long)duration_ms);
}

typedef struct serve_config {
  grpc_config config;
  int index;
} serve_config;

static void *serve(void *arg) {
  serve_config *cfg = arg;
  grpc_server *server = g_servers[cfg->index];

  grpc_server_handle(server, "/helloworld.Greeter/SayHello", say_hello, nullptr);
  grpc_server_handle(server, "/helloworld.Greeter/SayHelloAgain", say_hello_again, nullptr);
  grpc_server_handle(server, "/helloworld.Greeter/SayHelloStream", say_hello_stream, nullptr);
  grpc_server_handle_stream(server, "/helloworld.Greeter/SayHelloClientStream",
                               &(grpc_stream_handler){
                                   .on_message = client_stream_on_message,
                                   .on_end = client_stream_on_end,
                               },
                               nullptr);
  grpc_server_handle_stream(server, "/helloworld.Greeter/SayHelloBidiStream",
                               &(grpc_stream_handler){
                                   .on_message = bidi_on_message,
                                   .on_end = bidi_on_end,
                               },
                               nullptr);
  grpc_server_handle(server, "/helloworld.Greeter/SayHelloWithError", say_hello_with_error,
                        nullptr);
  grpc_server_handle(server, "/helloworld.Greeter/SayHelloSlow", say_hello_slow, nullptr);

  grpc_health_enable(server);
  grpc_reflection_enable(server);
  grpc_server_add_interceptor(server, &(grpc_interceptor){
                                             .name = "log",
                                             .on_call = log_call,
                                             .on_done = log_done,
                                         });

  grpc_server_run(server);
  return nullptr;
}

int main(int argc, char *argv[]) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  int port = argc > 1 ? atoi(argv[1]) : 19009;
  if (port <= 0 || port > 65535)
    port = 19009;
  g_quiet = argc > 2 ? atoi(argv[2]) : 0;
  bool use_tls = (getenv("USE_TLS") != nullptr && atoi(getenv("USE_TLS")) != 0) ||
                 (argc > 3 && atoi(argv[3]) != 0);

  grpc_config config = {
      .port = (uint16_t)port,
      .backlog = 1024,
      .log_level = g_quiet ? GRPC_LOG_WARNING : GRPC_LOG_INFO,
  };
  if (use_tls) {
    config.tls.cert_file = argc > 4 ? argv[4] : "server.crt";
    config.tls.key_file = argc > 5 ? argv[5] : "server.key";
  }

  /* number of event loops (one per thread) */
  int nb_threads = getenv("THREADS") != nullptr ? atoi(getenv("THREADS")) : 1;
  if (nb_threads < 1 || nb_threads > MAX_SERVERS)
    nb_threads = 1;
#ifdef _WIN32
  if (nb_threads > 1) {
    fprintf(stderr, "THREADS must be 1 on Windows (SO_REUSEPORT is unavailable)\n");
    return 1;
  }
#endif

  /* every server binds the same port, SO_REUSEPORT spreads the connections */
  for (; g_nb_servers < nb_threads; g_nb_servers++) {
    g_servers[g_nb_servers] = grpc_server_new(&config);
    if (g_servers[g_nb_servers] == nullptr) {
      fprintf(stderr, "Failed to create the server on port %d\n", port);
      for (int i = 0; i < g_nb_servers; i++)
        grpc_server_free(g_servers[i]);
      return 1;
    }
  }
  if (!g_quiet)
    printf("[server] Greeter listening on port %d%s with %d loop(s)\n", port,
           use_tls ? " (TLS)" : "", nb_threads);

#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  pthread_t threads[MAX_SERVERS];
  serve_config cfgs[MAX_SERVERS];
  for (int i = 1; i < g_nb_servers; i++) {
    cfgs[i] = (serve_config){.config = config, .index = i};
    pthread_create(&threads[i], nullptr, serve, &cfgs[i]);
  }
  cfgs[0] = (serve_config){.config = config, .index = 0};
  serve(&cfgs[0]);
  for (int i = 1; i < g_nb_servers; i++)
    pthread_join(threads[i], nullptr);

  printf("\n[server] Shutting down...\n");
  for (int i = 0; i < g_nb_servers; i++)
    grpc_server_free(g_servers[i]);
  return 0;
}
