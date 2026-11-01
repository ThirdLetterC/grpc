#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include "internal/grpc_threads.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <uv.h>
#include <nghttp2/nghttp2.h>

#include "grpc_platform.h"

#include "grpc.h"
#include "common.h"
#include "grpc_header.h"
#include "grpc_list.h"
#include "grpc_list_struct.h"
#include "grpc_messages.h"

/* ---- logging ------------------------------------------------------------- */

#ifndef _MSC_VER
[[gnu::format(printf, 3, 4)]]
#endif
void server_log(grpc_server *server, grpc_log_level level,
                                              const char *fmt, ...);

#define LOG_ERROR(server, fmt, ...) \
  server_log(server, GRPC_LOG_ERROR, "error: " fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_WARNING(server, fmt, ...) \
  server_log(server, GRPC_LOG_WARNING, "warning: " fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_INFO(server, ...)  server_log(server, GRPC_LOG_INFO, __VA_ARGS__)
#define LOG_DEBUG(server, ...) server_log(server, GRPC_LOG_DEBUG, __VA_ARGS__)
/* Per-frame logs are compiled in only with T=1. */
#ifdef GRPC_TRACE
#define LOG_TRACE(server, fmt, ...) \
  server_log(server, GRPC_LOG_TRACE, "@%s: " fmt, __func__ __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOG_TRACE(server, ...) \
  do {                         \
  } while (0)
#endif

/* ---- buffers --------------------------------------------------------------- */

/* A growable byte buffer. */
typedef struct grpc_buf grpc_buf;
struct grpc_buf {
  u8 *data;
  size_t len;
  size_t cap;
};

/* ---- routes ---------------------------------------------------------------- */

typedef enum route_kind {
  ROUTE_UNARY,  /* grpc_server_handle(). */
  ROUTE_STREAM, /* grpc_server_handle_stream() */
  ROUTE_EVENTS, /* grpc_server_register_path() */
} route_kind;

/* A registered method. It stays at a fixed address while the server lives
 * because calls hold pointers to it. */
typedef struct grpc_route grpc_route;
struct grpc_route {
  char *path;
  size_t path_len;
  route_kind kind;
  grpc_handler_fn handler;
  grpc_stream_handler stream;
  void *userdata;
  /* Disabled routes reject new calls with UNIMPLEMENTED. */
  bool disabled;
};

/* ---- calls ----------------------------------------------------------------- */

typedef struct grpc_stream grpc_stream;
typedef struct grpc_session grpc_session;

/* An owned list of response headers. */
typedef struct nv_list nv_list;
struct nv_list {
  nghttp2_nv *nva;
  size_t len;
  size_t cap;
};

struct grpc_call {
  atomic_size_t refs;
  grpc_server *server;
  grpc_route *route;

  /* The HTTP/2 stream, until it closes. */
  grpc_session *session;
  grpc_stream *stream;
  i32 stream_id;

  u64 start_ms;
  /* The client's deadline; 0 means there is none. */
  u64 deadline_ms;
  char peer[INET6_ADDRSTRLEN + 8];

  /* Header views and nghttp2 refs retained for the call lifetime. */
  grpc_header req_headers[CONFIG_STREAM_MAX_HEADERS];
  nghttp2_rcbuf *req_header_rcbufs[CONFIG_STREAM_MAX_HEADERS][2];
  size_t nb_req_headers;

  /* Received messages not yet delivered to the application. */
  grpc_list pending;
  /* The request for a ROUTE_UNARY call. */
  grpc_message *request;

  /* Response headers and trailers set by the application. */
  nv_list headers;
  nv_list trailers;

  grpc_status cancel_reason;

  bool headers_sent : 1;
  /* Encoding announced with the first compressed response message. */
  grpc_compression_algorithm compression;
  bool finished : 1;
  bool closed : 1;
  bool cancelled : 1;
  bool cancel_delivered : 1;
  /* The client half-closed its request stream. */
  bool half_closed : 1;
  bool end_delivered : 1;
  /* Interceptors ran and the application has seen the call. */
  bool started : 1;
  bool rejected : 1;
  bool request_delivered : 1;
  /* Interceptors' on_done callbacks ran. */
  bool done_reported : 1;
  bool queued : 1;

  void *userdata;
  void (*userdata_free)(void *);
  void (*cancel_fn)(grpc_call *, void *);
  void *cancel_userdata;

  /* Link in grpc_server.ready_head. */
  grpc_call *ready_next;
};

/* ---- streams and sessions ----------------------------------------------------- */

/* Small request messages are buffered inside the stream. */
enum { GRPC_STREAM_INLINE_RECV = 256 };

struct grpc_stream {
  i32 stream_id;

  /* Intrusive links in grpc_session.streams. */
  grpc_stream *prev;
  grpc_stream *next;

  size_t nb_headers;
  /* Names and values point into the nghttp2 buffers held by header_rcbufs. */
  grpc_header headers[CONFIG_STREAM_MAX_HEADERS];
  nghttp2_rcbuf *header_rcbufs[CONFIG_STREAM_MAX_HEADERS][2];

  bool is_method_post : 1;
  bool is_scheme_http : 1;
  /* True when the te header contains trailers. */
  bool is_te_trailers : 1;
  /* True when the content type starts with application/grpc. */
  bool is_content_grpc : 1;
  bool is_deadline_set : 1;
  /* A data provider was submitted and has not returned EOF. */
  bool is_data_active : 1;
  /* The request was rejected; ignore any remaining client data. */
  bool is_discarding : 1;
  uint64_t deadline_ms;

  /* These values point into headers. */
  const char *hcontent_type;
  const char *hgrpc_encoding;
  grpc_compression_algorithm grpc_encoding;
  const char *hgrpc_accept_encoding;
  const char *hgrpc_timeout;

  /* The method selected by :path. */
  grpc_route *route;
  /* The call for a routed, valid request. */
  grpc_call *call;

  /* recv_data points to recv_inline until the buffer needs to grow. */
  size_t recv_start;
  size_t recv_len;
  size_t recv_cap;
  u8 *recv_data;
  u8 recv_inline[GRPC_STREAM_INLINE_RECV];

  /* Bytes from the front message already handed to nghttp2, including its
   * five-byte prefix. */
  size_t trunc_seek;
  grpc_list lqueue_omessages;

  /* Trailers submitted while DATA is queued. They are sent after the data
   * provider reaches EOF so they cannot overtake the DATA. */
  nghttp2_nv *pending_trailers;
  size_t nb_pending_trailers;
};

struct grpc_session {
  nghttp2_session *ngsession;
  /* Index in grpc_server.sessions. */
  size_t index;
  socklen_t socklen;
  /* Parent server context. */
  grpc_server *server;

  struct sockaddr_storage sockaddr;
  /* Peer address in "ip:port" or "[ip]:port" form. */
  char peer[INET6_ADDRSTRLEN + 8];

  /* Open streams, allocated when a HEADERS frame is about to arrive. */
  grpc_stream *streams;
  size_t nb_streams;

  struct grpc_tls_session *tls;
  bool is_tls : 1;
  bool tls_handshake_done : 1;

  uv_tcp_t tcp;
  bool is_active : 1;
  bool is_closing : 1;

  /* Bytes serialized by nghttp2 (and encrypted, if TLS) but not yet accepted
   * by the kernel. */
  grpc_buf obuf;
  /* Buffer owned by the in-flight write_req. */
  grpc_buf wbuf;
  uv_write_t write_req;
  bool is_writing : 1;

  /* Link in grpc_server.flush_head. It survives a session reset because a
   * closed slot may still be queued. */
  bool is_flush_queued : 1;
  grpc_session *flush_next;
};

/* ---- the server ------------------------------------------------------------ */

typedef struct post_task post_task;
struct post_task {
  void (*fn)(void *);
  void *arg;
  post_task *next;
};

struct health_state;
struct reflection_state;

struct grpc_server {
  /* Configuration with defaults applied. */
  grpc_config config;
  char *ipv4_addr;
  char *ipv6_addr;

  atomic_int log_level; /* Current per-server threshold. */
  FILE *log_file;       /* Non-owned log stream. */
  char log_topic[40];
  int log_topic_id;
  bool log_topic_initialized;
  struct grpc_server *log_registry_next;

  grpc_route **routes;
  size_t nb_routes;
  size_t cap_routes;

  nghttp2_session_callbacks *ngcallbacks;

  uv_os_sock_t ipv4_sockfd;
  uv_os_sock_t ipv6_sockfd;

  uv_loop_t loop;
  uv_tcp_t ipv4_listener;
  uv_tcp_t ipv6_listener;
  /* Timeout used by grpc_server_poll(). */
  uv_timer_t timer;
  /* Fires at the earliest stream deadline. */
  uv_timer_t deadline_timer;
  /* Wakes the loop for grpc_server_post() and grpc_server_stop(). */
  uv_async_t async;

  bool loop_initialized : 1;
  bool ipv4_listener_active : 1;
  bool ipv6_listener_active : 1;
  bool timer_active : 1;
  bool deadline_timer_active : 1;
  bool async_active : 1;
  bool is_tls : 1;

  atomic_bool stop;

  int ready_events;

  /* One thread drives the loop. Each session reads into this buffer and
   * passes the bytes to nghttp2 before the next read. */
  u8 *rbuf;
  size_t rbuf_len;
  /* TLS plaintext output, batched for encryption. */
  grpc_buf tls_obuf;

  /* Sessions with output to flush on the next poll. */
  grpc_session *flush_head;

  /* Deadline for the timer; UINT64_MAX means it is disarmed. */
  u64 next_deadline_ms;

  size_t nb_sessions;
  grpc_session *sessions;
  /* Stack of indices for free slots in sessions. */
  size_t nb_free_sessions;
  size_t *free_sessions;

  size_t nb_interceptors;
  grpc_interceptor *interceptors;

  /* Calls with data to deliver to the application. */
  grpc_call *ready_head;
  grpc_call *ready_tail;

  /* Events from the last poll. Each holds a reference to its call and owns
   * its message. */
  grpc_event *events;
  size_t nb_events;
  size_t cap_events;

  /* Queue used by grpc_server_post(). */
  pthread_mutex_t post_mutex;
  post_task *post_head;
  post_task *post_tail;

  struct health_state *health;
  struct reflection_state *reflection;

  struct grpc_tls_ctx *tls_ctx;
};

/* ---- internal functions ------------------------------------------------------ */

/* grpc_call.c */
grpc_call *call_new(grpc_session *session, grpc_stream *stream);
/* Detach the stream and cancel the call unless it already finished. */
void call_detach(grpc_call *call, grpc_status reason);
/* End the call with status on behalf of the library and cancel it. */
void call_abort(grpc_call *call, grpc_status status, const char *message);
/* Queue the call for server_dispatch(). */
void call_queue(grpc_call *call);
/* Deliver queued calls to handlers or convert them into events. */
void server_dispatch(grpc_server *server);
/* Release events from the previous poll. */
void server_clear_events(grpc_server *server);
/* Drop queued calls without dispatching them. */
void server_drop_ready(grpc_server *server);

/* grpc_server.c */
grpc_route *server_find_route(grpc_server *server, const char *path, size_t len);
grpc_err server_add_route(grpc_server *server, const char *path, route_kind kind,
                             grpc_handler_fn handler, const grpc_stream_handler *stream,
                             void *userdata);

/* grpc_interceptor.c */
/* Run on_call interceptors; return false if one ends the call. */
bool interceptors_run_on_call(grpc_call *call);
void interceptors_run_on_done(grpc_call *call, grpc_status status);
void interceptors_free(grpc_server *server);

/* grpc_health.c, grpc_reflection.c */
void health_free(grpc_server *server);
void reflection_free(grpc_server *server);

/* grpc_tls.c */
grpc_err tls_enable(grpc_server *server, const char *cert_file, const char *key_file);
void tls_free(grpc_server *server);

/* grpc_message.c: a message with a compressed flag */
grpc_message *message_create(u8 cflag, const void *data, size_t len);
