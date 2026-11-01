/* The author disclaims copyright to this source code
 * and releases it into the public domain.
 *
 * Poll mode: the application runs the loop and handles the calls of its
 * methods as events, instead of registering handlers.
 *
 *   ./single_threaded.bin [port]
 *
 * Methods (raw bytes, no protobuf):
 *   /example.Echo/Echo     unary: replies with the request
 *   /example.Echo/Collect  client streaming: replies with the number of messages
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "grpc.h"

static volatile sig_atomic_t running = 1;

static void on_signal([[maybe_unused]] int sig) {
  running = 0;
}

enum method { ECHO, COLLECT };

static void handle_event(const grpc_event *event) {
  enum method method = *(enum method *)event->userdata;
  grpc_call *call = event->call;

  switch (event->type) {
  case GRPC_EVENT_MESSAGE:
    if (method == ECHO) {
      grpc_call_send_data(call, event->message->data, event->message->len);
    } else {
      /* per-call state lives on the call */
      size_t *count = grpc_call_userdata(call);
      if (count == nullptr) {
        count = calloc(1, sizeof(*count));
        grpc_call_set_userdata(call, count, free);
      }
      if (count != nullptr)
        (*count)++;
    }
    break;
  case GRPC_EVENT_END:
    if (method == COLLECT) {
      size_t *count = grpc_call_userdata(call);
      char reply[32];
      int len = snprintf(reply, sizeof(reply), "%zu", count != nullptr ? *count : 0);
      grpc_call_send_data(call, reply, (size_t)len);
    }
    grpc_call_finish(call, GRPC_STATUS_OK, nullptr);
    break;
  case GRPC_EVENT_CANCEL:
    printf("%s from %s cancelled: %s\n", grpc_call_method(call), grpc_call_peer(call),
           grpc_status_str(grpc_call_cancel_reason(call)));
    break;
  }
}

int main(int argc, char **argv) {
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  grpc_server *server = grpc_server_new(&(grpc_config){
      .port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 19009),
  });
  if (server == nullptr)
    return 1;

  static enum method echo = ECHO, collect = COLLECT;
  grpc_server_register_path(server, "/example.Echo/Echo", &echo);
  grpc_server_register_path(server, "/example.Echo/Collect", &collect);

  while (running) {
    int nb_events = grpc_server_poll(server, 500);
    if (nb_events < 0)
      break;

    size_t count;
    const grpc_event *events = grpc_server_events(server, &count);
    for (size_t i = 0; i < count; i++)
      handle_event(&events[i]);
  }

  grpc_server_free(server);
  return 0;
}
