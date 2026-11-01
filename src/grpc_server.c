#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"
#include "grpc_timeout.h"
#include "internal_helpers.h"
#include "internal_nghttp2_callbacks.h"
#include "ulog/ulog.h"

static const int DEFAULT_BACKLOG = 1024;
static const size_t DEFAULT_MAX_CONNECTIONS = 1024;
static const uint32_t DEFAULT_MAX_CONCURRENT_STREAMS = 1024;
static const uint32_t DEFAULT_MAX_MESSAGE_SIZE = 1u << 20;
static const uint32_t DEFAULT_MAX_FRAME_SIZE = 16u * 1024;

/* ---- logging ------------------------------------------------------------------ */

static pthread_mutex_t ulog_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_level_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t logger_once = PTHREAD_ONCE_INIT;
static grpc_server *log_registry;
static atomic_uint_fast64_t next_log_topic = 1;
static ulog_output_id log_output = ULOG_OUTPUT_INVALID;
static ulog_status logger_status = ULOG_STATUS_ERROR;

static ulog_status logger_lock(bool lock, [[maybe_unused]] void *arg) {
  int rc = lock ? pthread_mutex_lock(&ulog_mutex) : pthread_mutex_unlock(&ulog_mutex);
  return rc == 0 ? ULOG_STATUS_OK : ULOG_STATUS_BUSY;
}

static void logger_output(ulog_event *event, [[maybe_unused]] void *arg) {
  int topic_id = ulog_event_get_topic(event);
  char message[1024];
  if (topic_id == ULOG_TOPIC_ID_INVALID ||
      ulog_event_get_message(event, message, sizeof(message)) != ULOG_STATUS_OK)
    return;

  pthread_mutex_lock(&log_registry_mutex);
  grpc_server *server = log_registry;
  while (server != nullptr && server->log_topic_id != topic_id)
    server = server->log_registry_next;
  if (server != nullptr)
    (void)fwrite(message, 1, strlen(message), server->log_file);
  pthread_mutex_unlock(&log_registry_mutex);
}

static void logger_initialize(void) {
  if (ulog_lock_set_fn(logger_lock, nullptr) != ULOG_STATUS_OK)
    return;
  log_output = ulog_output_add(logger_output, nullptr, ULOG_LEVEL_TRACE);
  logger_status = log_output == ULOG_OUTPUT_INVALID ? ULOG_STATUS_ERROR : ULOG_STATUS_OK;
}

static ulog_level ulog_level_for(grpc_log_level level) {
  switch (level) {
  case GRPC_LOG_TRACE:
    return ULOG_LEVEL_TRACE;
  case GRPC_LOG_DEBUG:
    return ULOG_LEVEL_DEBUG;
  case GRPC_LOG_INFO:
    return ULOG_LEVEL_INFO;
  case GRPC_LOG_WARNING:
    return ULOG_LEVEL_WARN;
  case GRPC_LOG_ERROR:
  case GRPC_LOG_OFF:
  case GRPC_LOG_DEFAULT:
  default:
    return ULOG_LEVEL_ERROR;
  }
}

static grpc_err logger_server_add(grpc_server *server) {
  if (pthread_once(&logger_once, logger_initialize) != 0 || logger_status != ULOG_STATUS_OK)
    return GRPC_ERR_IO;

  uint_fast64_t topic = atomic_fetch_add(&next_log_topic, 1);
  snprintf(server->log_topic, sizeof(server->log_topic), "grpc-server-%" PRIuFAST64, topic);
  server->log_topic_id =
      ulog_topic_add(server->log_topic, log_output,
                     ulog_level_for((grpc_log_level)atomic_load(&server->log_level)));
  if (server->log_topic_id == ULOG_TOPIC_ID_INVALID)
    return GRPC_ERR_IO;

  pthread_mutex_lock(&log_registry_mutex);
  server->log_registry_next = log_registry;
  log_registry = server;
  server->log_topic_initialized = true;
  pthread_mutex_unlock(&log_registry_mutex);
  return GRPC_OK;
}

static void logger_server_remove(grpc_server *server) {
  if (!server->log_topic_initialized)
    return;

  (void)ulog_topic_remove(server->log_topic);
  pthread_mutex_lock(&log_registry_mutex);
  grpc_server **link = &log_registry;
  while (*link != nullptr && *link != server)
    link = &(*link)->log_registry_next;
  if (*link == server)
    *link = server->log_registry_next;
  server->log_topic_initialized = false;
  pthread_mutex_unlock(&log_registry_mutex);
}

void server_log(grpc_server *server, grpc_log_level level, const char *fmt, ...) {
  if (server == nullptr)
    return;
  int log_level = atomic_load(&server->log_level);
  if (log_level == GRPC_LOG_OFF || level > log_level)
    return;

  /* one write per line, so that lines of several servers do not interleave */
  char line[1024];
  char dt[128] = {};
  int n = snprintf(line, sizeof(line), "[%s] ", ezgetdt(dt, sizeof(dt)));
  va_list ap;
  va_start(ap, fmt);
  if (n >= 0 && (size_t)n < sizeof(line))
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
  va_end(ap);

  switch (level) {
  case GRPC_LOG_ERROR:
    ulog_t_error(server->log_topic, "%s", line);
    break;
  case GRPC_LOG_WARNING:
    ulog_t_warn(server->log_topic, "%s", line);
    break;
  case GRPC_LOG_INFO:
    ulog_t_info(server->log_topic, "%s", line);
    break;
  case GRPC_LOG_DEBUG:
    ulog_t_debug(server->log_topic, "%s", line);
    break;
  case GRPC_LOG_TRACE:
    ulog_t_trace(server->log_topic, "%s", line);
    break;
  default:
    break;
  }
}

/* ---- routes ------------------------------------------------------------------- */

grpc_route *server_find_route(grpc_server *server, const char *path, size_t len) {
  for (size_t i = 0; i < server->nb_routes; i++) {
    grpc_route *route = server->routes[i];
    if (route->path_len == len && memcmp(route->path, path, len) == 0)
      return route;
  }
  return nullptr;
}

grpc_err server_add_route(grpc_server *server, const char *path, route_kind kind,
                             grpc_handler_fn handler, const grpc_stream_handler *stream,
                             void *userdata) {
  if (server == nullptr || path == nullptr || path[0] != '/')
    return GRPC_ERR_INVALID;

  size_t len = strlen(path);
  grpc_route *route = server_find_route(server, path, len);
  if (route != nullptr) {
    if (!route->disabled)
      return GRPC_ERR_EXISTS;
  } else {
    /* routes are never moved: calls point to them */
    if (server->nb_routes == server->cap_routes) {
      size_t cap = server->cap_routes ? server->cap_routes * 2 : 16;
      grpc_route **routes = grpc_realloc(server->routes, cap * sizeof(*routes));
      if (routes == nullptr)
        return GRPC_ERR_NOMEM;
      server->routes = routes;
      server->cap_routes = cap;
    }
    route = grpc_calloc(1, sizeof(*route));
    if (route == nullptr)
      return GRPC_ERR_NOMEM;
    route->path = grpc_strndup(path, len);
    if (route->path == nullptr) {
      grpc_free(route);
      return GRPC_ERR_NOMEM;
    }
    route->path_len = len;
    server->routes[server->nb_routes++] = route;
  }

  route->kind = kind;
  route->handler = handler;
  route->stream = stream != nullptr ? *stream : (grpc_stream_handler){};
  route->userdata = userdata;
  route->disabled = false;
  return GRPC_OK;
}

GRPC_API grpc_err grpc_server_handle(grpc_server *server, const char *path,
                                              grpc_handler_fn handler, void *userdata) {
  if (handler == nullptr)
    return GRPC_ERR_INVALID;
  return server_add_route(server, path, ROUTE_UNARY, handler, nullptr, userdata);
}

GRPC_API grpc_err grpc_server_handle_stream(grpc_server *server, const char *path,
                                                     const grpc_stream_handler *handler,
                                                     void *userdata) {
  if (handler == nullptr || handler->on_message == nullptr)
    return GRPC_ERR_INVALID;
  return server_add_route(server, path, ROUTE_STREAM, nullptr, handler, userdata);
}

GRPC_API grpc_err grpc_server_register_path(grpc_server *server, const char *path,
                                                     void *userdata) {
  return server_add_route(server, path, ROUTE_EVENTS, nullptr, nullptr, userdata);
}

GRPC_API grpc_err grpc_server_unregister(grpc_server *server, const char *path) {
  if (server == nullptr || path == nullptr)
    return GRPC_ERR_INVALID;
  grpc_route *route = server_find_route(server, path, strlen(path));
  if (route == nullptr || route->disabled)
    return GRPC_ERR_NOT_FOUND;
  /* kept, since calls in progress point to it */
  route->disabled = true;
  return GRPC_OK;
}

/* ---- setup ---------------------------------------------------------------------- */

#ifdef _WIN32
static void grpc_winsock_init(void) {
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
}
#endif

/* binds and listens on one address. returns the socket or -1 */
static uv_os_sock_t open_listener(grpc_server *server, int family, const char *addr, uint16_t port,
                                  int backlog) {
  struct sockaddr_storage ss = {};
  int sslen;
  if (family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &sin->sin_addr) != 1) {
      LOG_ERROR(server, "invalid IPv4 address '%s'\n", addr);
      return GRPC_INVALID_SOCKET;
    }
    sslen = sizeof(*sin);
  } else {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(port);
    if (inet_pton(AF_INET6, addr, &sin6->sin6_addr) != 1) {
      LOG_ERROR(server, "invalid IPv6 address '%s'\n", addr);
      return GRPC_INVALID_SOCKET;
    }
    sslen = sizeof(*sin6);
  }

#ifdef _WIN32
  static uv_once_t winsock_once = UV_ONCE_INIT;
  uv_once(&winsock_once, grpc_winsock_init);
  uv_os_sock_t fd = WSASocketW(family, SOCK_STREAM, 0, NULL, 0,
                              WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
  uv_os_sock_t fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
  if (fd == GRPC_INVALID_SOCKET) {
    LOG_ERROR(server, "socket: %s\n", grpc_socket_error());
    return GRPC_INVALID_SOCKET;
  }
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&(int){1}, sizeof(int));
#ifdef SO_REUSEPORT
  /* lets one server per thread share the port; the kernel spreads connections */
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&(int){1}, sizeof(int));
#endif
  if (family == AF_INET6)
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&(int){1}, sizeof(int));

  if (bind(fd, (struct sockaddr *)&ss, sslen) == -1) {
    LOG_ERROR(server, "bind %s port %u: %s\n", addr, port, grpc_socket_error());
    grpc_socket_close(fd);
    return GRPC_INVALID_SOCKET;
  }
  if (listen(fd, backlog) == -1) {
    LOG_ERROR(server, "listen %s port %u: %s\n", addr, port, grpc_socket_error());
    grpc_socket_close(fd);
    return GRPC_INVALID_SOCKET;
  }
  return fd;
}

/* hands an open socket to an initialized libuv listener, which then owns it */
static bool start_listener(grpc_server *server, uv_tcp_t *listener, uv_os_sock_t *fd) {
  listener->data = server;
  if (uv_tcp_open(listener, *fd) != 0)
    return false;
  *fd = GRPC_INVALID_SOCKET;
  return uv_listen((uv_stream_t *)listener, server->config.backlog, on_listener_connection) == 0;
}

static void on_poll_timer([[maybe_unused]] uv_timer_t *handle) {}

static void run_posted(grpc_server *server) {
  pthread_mutex_lock(&server->post_mutex);
  post_task *task = server->post_head;
  server->post_head = server->post_tail = nullptr;
  pthread_mutex_unlock(&server->post_mutex);

  while (task != nullptr) {
    post_task *next = task->next;
    task->fn(task->arg);
    grpc_free(task);
    task = next;
  }
}

static void on_async(uv_async_t *handle) {
  grpc_server *server = handle->data;
  server->ready_events++;
  run_posted(server);
}

GRPC_API grpc_server *grpc_server_new(const grpc_config *config) {
  if (config == nullptr || config->port == 0) {
    fprintf(config != nullptr && config->log_file != nullptr ? config->log_file : stderr,
            "grpc_server_new: a port is required\n");
    return nullptr;
  }

  grpc_server *server = grpc_calloc(1, sizeof(*server));
  if (server == nullptr)
    return nullptr;
  server->ipv4_sockfd = GRPC_INVALID_SOCKET;
  server->ipv6_sockfd = GRPC_INVALID_SOCKET;
  server->next_deadline_ms = UINT64_MAX;
  pthread_mutex_init(&server->post_mutex, nullptr);

  /* the configuration, with defaults */
  grpc_config *cfg = &server->config;
  *cfg = *config;
  if (cfg->backlog <= 0)
    cfg->backlog = DEFAULT_BACKLOG;
  if (cfg->max_connections == 0)
    cfg->max_connections = DEFAULT_MAX_CONNECTIONS;
  if (cfg->max_concurrent_streams == 0)
    cfg->max_concurrent_streams = DEFAULT_MAX_CONCURRENT_STREAMS;
  if (cfg->max_message_size == 0)
    cfg->max_message_size = DEFAULT_MAX_MESSAGE_SIZE;
  if (cfg->max_frame_size == 0)
    cfg->max_frame_size = DEFAULT_MAX_FRAME_SIZE;
  grpc_log_level initial_level =
      cfg->log_level == GRPC_LOG_DEFAULT ? GRPC_LOG_INFO : cfg->log_level;
  if (initial_level < GRPC_LOG_OFF || initial_level > GRPC_LOG_TRACE) {
    fprintf(stderr, "grpc: invalid log level\n");
    goto err;
  }
  atomic_init(&server->log_level, initial_level);
  server->log_file = cfg->log_file != nullptr ? cfg->log_file : stderr;
  cfg->log_file = server->log_file;
  if (logger_server_add(server) != GRPC_OK)
    goto err;

  /* HTTP/2 limits: a frame is 16 KiB to 16 MiB, a window at most 2^31 - 1 */
  if (cfg->max_frame_size < (16u * 1024) || cfg->max_frame_size > (1u << 24) - 1 ||
      cfg->max_message_size > INT32_MAX) {
    LOG_ERROR(server, "max_frame_size or max_message_size is out of range\n");
    goto err;
  }

  const char *ipv4_addr = cfg->ipv4_addr, *ipv6_addr = cfg->ipv6_addr;
  if (ipv4_addr == nullptr && ipv6_addr == nullptr) {
    ipv4_addr = "0.0.0.0";
    ipv6_addr = "::";
  }
  if ((ipv4_addr != nullptr && (server->ipv4_addr = grpc_strdup(ipv4_addr)) == nullptr) ||
      (ipv6_addr != nullptr && (server->ipv6_addr = grpc_strdup(ipv6_addr)) == nullptr))
    goto err;
  /* the copies outlive the caller's strings */
  cfg->ipv4_addr = server->ipv4_addr;
  cfg->ipv6_addr = server->ipv6_addr;
  cfg->tls.cert_file = cfg->tls.key_file = nullptr;

  if ((config->tls.cert_file == nullptr) != (config->tls.key_file == nullptr)) {
    LOG_ERROR(server, "TLS needs both tls.cert_file and tls.key_file\n");
    goto err;
  }
  if (config->tls.cert_file != nullptr &&
      tls_enable(server, config->tls.cert_file, config->tls.key_file) != GRPC_OK)
    goto err;

  if (server->ipv4_addr != nullptr &&
      (server->ipv4_sockfd = open_listener(server, AF_INET, server->ipv4_addr, cfg->port,
                                           cfg->backlog)) == GRPC_INVALID_SOCKET)
    goto err;
  if (server->ipv6_addr != nullptr &&
      (server->ipv6_sockfd = open_listener(server, AF_INET6, server->ipv6_addr, cfg->port,
                                           cfg->backlog)) == GRPC_INVALID_SOCKET)
    goto err;

  if (uv_loop_init(&server->loop) != 0)
    goto err;
  server->loop_initialized = true;

  if (uv_timer_init(&server->loop, &server->timer) != 0)
    goto err;
  server->timer_active = true;
  if (uv_timer_init(&server->loop, &server->deadline_timer) != 0)
    goto err;
  server->deadline_timer.data = server;
  server->deadline_timer_active = true;
  if (uv_async_init(&server->loop, &server->async, on_async) != 0)
    goto err;
  server->async.data = server;
  server->async_active = true;

  if (server->ipv4_sockfd != GRPC_INVALID_SOCKET) {
    if (uv_tcp_init(&server->loop, &server->ipv4_listener) != 0)
      goto err;
    server->ipv4_listener_active = true;
    if (!start_listener(server, &server->ipv4_listener, &server->ipv4_sockfd))
      goto err;
  }
  if (server->ipv6_sockfd != GRPC_INVALID_SOCKET) {
    if (uv_tcp_init(&server->loop, &server->ipv6_listener) != 0)
      goto err;
    server->ipv6_listener_active = true;
    if (!start_listener(server, &server->ipv6_listener, &server->ipv6_sockfd))
      goto err;
  }

  if (nghttp2_session_callbacks_new(&server->ngcallbacks) != 0)
    goto err;
  server_setup_ngcallbacks(server->ngcallbacks);

  server->rbuf_len = 64 * 1024;
  server->rbuf = grpc_malloc(server->rbuf_len);
  if (server->rbuf == nullptr)
    goto err;

  server->nb_sessions = cfg->max_connections;
  server->sessions = grpc_calloc(server->nb_sessions, sizeof(*server->sessions));
  server->free_sessions = grpc_malloc(server->nb_sessions * sizeof(*server->free_sessions));
  if (server->sessions == nullptr || server->free_sessions == nullptr)
    goto err;
  /* lowest index on top */
  for (size_t i = 0; i < server->nb_sessions; i++)
    server->free_sessions[i] = server->nb_sessions - 1 - i;
  server->nb_free_sessions = server->nb_sessions;

  if (server->ipv4_addr != nullptr)
    LOG_INFO(server, "listening on %s:%u%s\n", server->ipv4_addr, cfg->port,
             server->is_tls ? " (TLS)" : "");
  if (server->ipv6_addr != nullptr)
    LOG_INFO(server, "listening on [%s]:%u%s\n", server->ipv6_addr, cfg->port,
             server->is_tls ? " (TLS)" : "");
  return server;

err:
  grpc_server_free(server);
  return nullptr;
}

GRPC_API void grpc_server_free(grpc_server *server) {
  if (server == nullptr)
    return;

  if (server->sessions != nullptr) {
    /* deliver what the application already queued */
    server_flush_sessions(server);
    for (size_t i = 0; i < server->nb_sessions; i++) {
      if (server->sessions[i].is_active)
        session_close(&server->sessions[i]);
    }
  }
  /* posted work runs (on calls that are now closed) so that it releases its references */
  run_posted(server);
  server_drop_ready(server);
  server_clear_events(server);

  if (server->ipv4_listener_active)
    uv_close((uv_handle_t *)&server->ipv4_listener, nullptr);
  if (server->ipv6_listener_active)
    uv_close((uv_handle_t *)&server->ipv6_listener, nullptr);
  if (server->timer_active)
    uv_close((uv_handle_t *)&server->timer, nullptr);
  if (server->deadline_timer_active)
    uv_close((uv_handle_t *)&server->deadline_timer, nullptr);
  if (server->async_active)
    uv_close((uv_handle_t *)&server->async, nullptr);
  if (server->ipv4_sockfd != GRPC_INVALID_SOCKET)
    grpc_socket_close(server->ipv4_sockfd);
  if (server->ipv6_sockfd != GRPC_INVALID_SOCKET)
    grpc_socket_close(server->ipv6_sockfd);

  if (server->loop_initialized) {
    int drain_limit = 1000;
    while (uv_loop_alive(&server->loop) && drain_limit-- > 0)
      uv_run(&server->loop, UV_RUN_NOWAIT);
    uv_loop_close(&server->loop);
  }

  for (size_t i = 0; i < server->nb_routes; i++) {
    grpc_free(server->routes[i]->path);
    grpc_free(server->routes[i]);
  }
  grpc_free(server->routes);
  interceptors_free(server);
  health_free(server);
  reflection_free(server);
  tls_free(server);

  if (server->ngcallbacks != nullptr)
    nghttp2_session_callbacks_del(server->ngcallbacks);
  grpc_free(server->events);
  grpc_free(server->rbuf);
  grpc_free(server->tls_obuf.data);
  grpc_free(server->ipv4_addr);
  grpc_free(server->ipv6_addr);
  grpc_free(server->sessions);
  grpc_free(server->free_sessions);
  logger_server_remove(server);
  pthread_mutex_destroy(&server->post_mutex);
  grpc_free(server);
}

GRPC_API grpc_err grpc_server_set_log_level(grpc_server *server,
                                                     grpc_log_level level) {
  if (server == nullptr || level < GRPC_LOG_DEFAULT || level > GRPC_LOG_TRACE)
    return GRPC_ERR_INVALID;
  if (level == GRPC_LOG_DEFAULT)
    level = GRPC_LOG_INFO;

  pthread_mutex_lock(&log_level_mutex);
  ulog_status status = ulog_topic_level_set(server->log_topic, ulog_level_for(level));
  if (status == ULOG_STATUS_OK)
    atomic_store(&server->log_level, level);
  pthread_mutex_unlock(&log_level_mutex);
  return status == ULOG_STATUS_OK ? GRPC_OK : GRPC_ERR_IO;
}

/* ---- running ---------------------------------------------------------------------- */

GRPC_API int grpc_server_poll(grpc_server *server, int timeout_ms) {
  if (server == nullptr)
    return GRPC_ERR_INVALID;

  server_clear_events(server);
  server->ready_events = 0;

  /* write everything queued since the last poll, one write per connection */
  server_flush_sessions(server);

  if (timeout_ms > 0) {
    uv_timer_start(&server->timer, on_poll_timer, (uint64_t)timeout_ms, 0);
    uv_run(&server->loop, UV_RUN_ONCE);
    uv_timer_stop(&server->timer);
  } else if (timeout_ms == 0) {
    uv_run(&server->loop, UV_RUN_NOWAIT);
  } else {
    uv_run(&server->loop, UV_RUN_ONCE);
  }

  server_dispatch(server);
  /* what the handlers answered */
  server_flush_sessions(server);
  return (int)server->nb_events;
}

GRPC_API const grpc_event *grpc_server_events(grpc_server *server, size_t *count) {
  if (server == nullptr) {
    if (count != nullptr)
      *count = 0;
    return nullptr;
  }
  if (count != nullptr)
    *count = server->nb_events;
  return server->events;
}

GRPC_API grpc_err grpc_server_run(grpc_server *server) {
  if (server == nullptr)
    return GRPC_ERR_INVALID;
  while (!atomic_load(&server->stop)) {
    int res = grpc_server_poll(server, -1);
    if (res < 0)
      return (grpc_err)res;
  }
  /* the server can be run again */
  atomic_store(&server->stop, false);
  return GRPC_OK;
}

GRPC_API void grpc_server_stop(grpc_server *server) {
  if (server == nullptr)
    return;
  atomic_store(&server->stop, true);
  /* async-signal-safe */
  uv_async_send(&server->async);
}

GRPC_API grpc_err grpc_server_post(grpc_server *server, void (*fn)(void *arg),
                                            void *arg) {
  if (server == nullptr || fn == nullptr)
    return GRPC_ERR_INVALID;
  post_task *task = grpc_malloc(sizeof(*task));
  if (task == nullptr)
    return GRPC_ERR_NOMEM;
  *task = (post_task){.fn = fn, .arg = arg};

  pthread_mutex_lock(&server->post_mutex);
  if (server->post_tail != nullptr)
    server->post_tail->next = task;
  else
    server->post_head = task;
  server->post_tail = task;
  pthread_mutex_unlock(&server->post_mutex);

  uv_async_send(&server->async);
  return GRPC_OK;
}

GRPC_API void grpc_server_flush(grpc_server *server) {
  if (server != nullptr)
    server_flush_sessions(server);
}

GRPC_API const char *grpc_license(void) {
  static const char *license =
      "grpc - A grpc server without the extra fancy features.\n"
      "https://github.com/mnyoshie/ezgrpc2\n"
      "\n"
      "Copyright (c) 2023-2025 M. N. Yoshie & Al-buharie Amjari\n"
      "\n"
      "Redistribution and use in source and binary forms, with or without\n"
      "modification, are permitted provided that the following conditions\n"
      "are met:\n"
      "\n"
      "  1. Redistributions of source code must retain the above copyright notice,\n"
      "  this list of conditions and the following disclaimer.\n"
      "\n"
      "  2. Neither the name of the copyright holder nor the names of its\n"
      "  contributors may be used to endorse or promote products derived from\n"
      "  this software without specific prior written permission.\n"
      "\n"
      "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
      "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
      "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
      "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
      "HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
      "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED\n"
      "TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR\n"
      "PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF\n"
      "LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING\n"
      "NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS\n"
      "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n";
  return license;
}
