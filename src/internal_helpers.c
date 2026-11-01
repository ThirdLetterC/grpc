#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <time.h>
#include <stddef.h>
#include "internal/grpc_platform.h"
#include <stdio.h>
#include "core.h"
#include "grpc_header.h"
#include "internal_nghttp2_callbacks.h"
#include "internal_helpers.h"
#include "internal_tls.h"

int list_cmp_ezheader_name(const void *data, const void *userdata) {
  const grpc_header *a = data;
  const grpc_header *b = userdata;
  return a->namelen == b->namelen ? strncasecmp(a->name, b->name, a->namelen) : 1;
}

static void dump_decode_binary(const i8 *data, size_t len) {
  const i8 look_up[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (size_t i = 0; i < len; i++) {
    putchar(look_up[(data[i] >> 4) & 0x0f]);
    putchar(look_up[data[i] & 0x0f]);
    putchar(' ');
    if (i == 7)
      putchar(' ');
  }
}

static void dump_decode_ascii(const i8 *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x20) {
      putchar('.');
    } else if (data[i] < 0x7f)
      putchar(data[i]);
    else
      putchar('.');
  }
}

void ezdump(void *vdata, size_t len) {
  const i8 *data = vdata;
  if (len == 0)
    return;

  size_t cur = 0;

  for (; cur < 16 * (len / 16); cur += 16) {
    dump_decode_binary(data + cur, 16);
    printf("| ");
    dump_decode_ascii(data + cur, 16);
    printf(" |");
    puts("");
  }
  /* write the remaining */
  if (len % 16) {
    dump_decode_binary(data + cur, len % 16);
    /* write the empty */
    for (size_t i = 0; i < 16 - (len % 16); i++) {
      printf("   ");
      if (i == 7 && (7 < 16 - (len % 16)))
        putchar(' ');
    }
    printf("| ");
    dump_decode_ascii(data + cur, len % 16);
    for (size_t i = 0; i < 16 - (len % 16); i++) {
      putchar(' ');
    }
    printf(" |");
    puts("");
    cur += len % 16;
  }
  assert(len == cur);
}

char *ezgetdt(char *buf, size_t len) {
#ifdef __unix__
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  char tmp[32] = {};
  struct tm stm;
  localtime_r(&ts.tv_sec, &stm);
  strftime(tmp, sizeof(tmp), "%F %T", &stm);
  snprintf(buf, len, "%s.%09ld", tmp, ts.tv_nsec);
#else
  time_t t = time(nullptr);
  struct tm stm;
#ifdef _WIN32
  localtime_s(&stm, &t);
#else
  localtime_r(&t, &stm);
#endif
  snprintf(buf, len, "%d/%02d/%02d %02d:%02d:%02d", stm.tm_year + 1900, stm.tm_mon + 1, stm.tm_mday,
           stm.tm_hour, stm.tm_min, stm.tm_sec);
#endif
  return buf;
}

void ezlog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char dt[128] = {};
  fprintf(stderr, "[%s] ", ezgetdt(dt, sizeof(dt)));
  vfprintf(stderr, fmt, args);
  va_end(args);
}

void ezflog(FILE *fp, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char dt[128] = {};
  fprintf(fp, "[%s] ", ezgetdt(dt, sizeof(dt)));
  vfprintf(fp, fmt, args);
  va_end(args);
}

/* stop pulling frames out of nghttp2 once this many bytes wait for the kernel */
static const size_t GRPC_OBUF_HIGH_WATER = 256 * 1024;

static void on_session_close(uv_handle_t *handle) {
  grpc_session *session = handle->data;
  grpc_server *server = session->server;
  size_t ndx = session->index;

  buf_free(&session->obuf);
  buf_free(&session->wbuf);
  session->nb_streams = 0;
  session->tls_handshake_done = false;
  session->is_writing = false;
  memset(&session->sockaddr, 0, sizeof(session->sockaddr));
  session->peer[0] = '\0';
  session->is_closing = false;
  session->is_active = false;

  server->free_sessions[server->nb_free_sessions++] = ndx;
}

void session_close(grpc_session *session) {
  if (session == nullptr || !session->is_active || session->is_closing) {
    return;
  }
  session->is_closing = true;
  LOG_DEBUG(session->server, "closing connection from %s\n", session->peer);

  uv_read_stop((uv_stream_t *)&session->tcp);

  /* before nghttp2_session_del(): the streams hold nghttp2 buffers that are
   * released through the session */
  grpc_stream *ezstream;
  while ((ezstream = session->streams) != nullptr) {
    stream_unlink(session, ezstream);
    /* the client is gone: its calls are cancelled */
    if (ezstream->call != nullptr)
      call_detach(ezstream->call, GRPC_STATUS_UNAVAILABLE);
    stream_free(ezstream);
  }

  if (session->ngsession != nullptr) {
    nghttp2_session_del(session->ngsession);
    session->ngsession = nullptr;
  }

  if (session->is_tls) {
    /* best effort close_notify. skipped if it would overtake queued output */
    if (session->tls_handshake_done && !session->is_writing) {
      session->obuf.len = 0;
      grpc_tls_close_notify(session);
      if (!grpc_tls_drain(session, &session->obuf) && session->obuf.len > 0) {
        uv_buf_t buf = uv_buf_init((char *)session->obuf.data, (unsigned int)session->obuf.len);
        (void)uv_try_write((uv_stream_t *)&session->tcp, &buf, 1);
      }
    }
    grpc_tls_session_free(session);
    session->is_tls = false;
  }

  /* pending writes are cancelled before on_session_close runs */
  uv_close((uv_handle_t *)&session->tcp, on_session_close);
}

static void *nghttp2_mem_malloc(size_t size, [[maybe_unused]] void *user_data) {
  return grpc_malloc(size);
}

static void nghttp2_mem_free(void *ptr, [[maybe_unused]] void *user_data) {
  grpc_free(ptr);
}

static void *nghttp2_mem_calloc(size_t nmemb, size_t size, [[maybe_unused]] void *user_data) {
  return grpc_calloc(nmemb, size);
}

static void *nghttp2_mem_realloc(void *ptr, size_t size, [[maybe_unused]] void *user_data) {
  return grpc_realloc(ptr, size);
}

/* sets up TLS and nghttp2 for an accepted connection */
static int session_create(grpc_session *ezsession) {
  grpc_server *server = ezsession->server;

  /* TLS goes first so that a failure never leaves an announced session behind */
  if (server->is_tls && grpc_tls_session_init(ezsession) != 0) {
    return 1;
  }

  nghttp2_mem mem = {
      .mem_user_data = nullptr,
      .malloc = nghttp2_mem_malloc,
      .free = nghttp2_mem_free,
      .calloc = nghttp2_mem_calloc,
      .realloc = nghttp2_mem_realloc,
  };
  int res = nghttp2_session_server_new3(&ezsession->ngsession, server->ngcallbacks, ezsession,
                                        nullptr, &mem);
  if (res < 0) {
    ezsession->ngsession = nullptr;
    return 1;
  }

  /* settings */
  nghttp2_settings_entry siv[] = {
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, server->config.max_message_size},
      {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, server->config.max_frame_size},
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
       server->config.max_concurrent_streams},
  };
  res = nghttp2_submit_settings(ezsession->ngsession, NGHTTP2_FLAG_NONE, siv, 3);
  if (res < 0) {
    return 1;
  }

  return 0;
}

static void session_set_peer(grpc_session *ezsession) {
  int namelen = sizeof(ezsession->sockaddr);
  if (uv_tcp_getpeername(&ezsession->tcp, (struct sockaddr *)&ezsession->sockaddr, &namelen) != 0) {
    return;
  }
  ezsession->socklen = namelen;
  char addr[INET6_ADDRSTRLEN] = "";
  switch (ezsession->sockaddr.ss_family) {
  case AF_INET: {
    struct sockaddr_in *sa = (struct sockaddr_in *)&ezsession->sockaddr;
    inet_ntop(AF_INET, &sa->sin_addr.s_addr, addr, sizeof(addr));
    snprintf(ezsession->peer, sizeof(ezsession->peer), "%s:%u", addr, ntohs(sa->sin_port));
  } break;
  case AF_INET6: {
    struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ezsession->sockaddr;
    inet_ntop(AF_INET6, sa6->sin6_addr.s6_addr, addr, sizeof(addr));
    snprintf(ezsession->peer, sizeof(ezsession->peer), "[%s]:%u", addr, ntohs(sa6->sin6_port));
  } break;
  default:
    break;
  }
}

static void on_session_alloc(uv_handle_t *handle, [[maybe_unused]] size_t suggested_size,
                             uv_buf_t *buf) {
  grpc_server *server = ((grpc_session *)handle->data)->server;
  *buf = uv_buf_init((char *)server->rbuf, (unsigned int)server->rbuf_len);
}

static int session_recv(grpc_session *ezsession, const u8 *data, size_t len) {
  ssize_t res = nghttp2_session_mem_recv(ezsession->ngsession, data, len);
  if (res < 0) {
    LOG_ERROR(ezsession->server, "@ %s: nghttp2: %s. killing...\n", __func__,
                      nghttp2_strerror((int)res));
    return 1;
  }
  return 0;
}

static int session_recv_tls(grpc_session *ezsession, const u8 *data, size_t len) {
  grpc_server *server = ezsession->server;
  if (grpc_tls_feed(ezsession, data, len)) {
    return 1;
  }

  if (!ezsession->tls_handshake_done) {
    int hres = grpc_tls_handshake(ezsession);
    if (hres < 0) {
      LOG_ERROR(server, "TLS handshake failed on session %p\n", (void *)ezsession);
      return 1;
    }
    if (hres == 0) {
      return 0; // Handshake in progress
    }
  }

  /* the ciphertext now lives in the read BIO, so the read buffer can hold the plaintext */
  for (;;) {
    ssize_t n = grpc_tls_read(ezsession, server->rbuf, server->rbuf_len);
    if (n == -2) {
      return 0;
    }
    if (n <= 0) {
      return 1;
    }
    if (session_recv(ezsession, server->rbuf, (size_t)n)) {
      return 1;
    }
  }
}

static void on_session_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  grpc_session *ezsession = stream->data;
  if (nread == 0 || !ezsession->is_active || ezsession->is_closing) {
    return;
  }
  ezsession->server->ready_events++;

  if (nread < 0) {
    if (nread != UV_EOF) {
      LOG_DEBUG(ezsession->server, "read: %s\n", uv_strerror((int)nread));
    }
    session_close(ezsession);
    return;
  }

  int res = ezsession->is_tls ? session_recv_tls(ezsession, (const u8 *)buf->base, (size_t)nread)
                              : session_recv(ezsession, (const u8 *)buf->base, (size_t)nread);
  if (res) {
    LOG_TRACE(ezsession->server, "closing session %p", (void *)ezsession);
    session_close(ezsession);
    return;
  }
  /* nghttp2 may want to answer (SETTINGS ACK, WINDOW_UPDATE, ...) */
  session_schedule_flush(ezsession);
}

static void on_reject_close(uv_handle_t *handle) {
  grpc_free(handle);
}

/* libuv stops accepting until the pending connection is taken, so take it and drop it */
static void reject_connection(grpc_server *server, uv_stream_t *listener) {
  uv_tcp_t *tcp = grpc_malloc(sizeof(*tcp));
  if (tcp == nullptr) {
    return;
  }
  if (uv_tcp_init(&server->loop, tcp) != 0) {
    grpc_free(tcp);
    return;
  }
  (void)uv_accept(listener, (uv_stream_t *)tcp);
  uv_close((uv_handle_t *)tcp, on_reject_close);
}

void on_listener_connection(uv_stream_t *listener, int status) {
  grpc_server *server = listener->data;
  if (status < 0) {
    LOG_ERROR(server, "accept: %s\n", uv_strerror(status));
    return;
  }

  if (server->nb_free_sessions == 0) {
    LOG_WARNING(server, "max_connections (%zu) reached, rejecting a connection\n",
                server->nb_sessions);
    reject_connection(server, listener);
    return;
  }

  size_t ndx = server->free_sessions[server->nb_free_sessions - 1];
  grpc_session *session = &server->sessions[ndx];
  if (uv_tcp_init(&server->loop, &session->tcp) != 0) {
    reject_connection(server, listener);
    return;
  }
  server->nb_free_sessions--;
  session->tcp.data = session;
  session->server = server;
  session->index = ndx;
  session->is_active = true;
  session->is_closing = false;
  /* from here on, session_close() returns the slot */

  if (uv_accept(listener, (uv_stream_t *)&session->tcp) != 0) {
    session_close(session);
    return;
  }
  server->ready_events++;

  uv_os_fd_t fd;
  if (uv_fileno((uv_handle_t *)&session->tcp, &fd) == 0)
    (void)setsockopt((uv_os_sock_t)(uintptr_t)fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&(int){1},
                     sizeof(int));
  (void)uv_tcp_nodelay(&session->tcp, 1);
  session_set_peer(session);
  LOG_DEBUG(server, "connection from %s\n", session->peer);

  if (session_create(session)) {
    LOG_ERROR(server, "cannot set up the connection from %s\n", session->peer);
    session_close(session);
    return;
  }

  if (uv_read_start((uv_stream_t *)&session->tcp, on_session_alloc, on_session_read) != 0) {
    session_close(session);
    return;
  }

  /* our SETTINGS frame */
  session_schedule_flush(session);
}

void session_schedule_flush(grpc_session *session) {
  if (session->is_flush_queued)
    return;
  session->is_flush_queued = true;
  session->flush_next = session->server->flush_head;
  session->server->flush_head = session;
}

void server_flush_sessions(grpc_server *server) {
  grpc_session *session;
  while ((session = server->flush_head) != nullptr) {
    server->flush_head = session->flush_next;
    session->flush_next = nullptr;
    session->is_flush_queued = false;
    session_flush(session);
  }
}

/* moves the frames queued in nghttp2 into obuf, encrypting them if needed */
static int session_serialize(grpc_session *ezsession) {
  /* TLS application data cannot be written before the handshake completes */
  bool can_send = !ezsession->is_tls || ezsession->tls_handshake_done;
  grpc_buf *out = ezsession->is_tls ? &ezsession->server->tls_obuf : &ezsession->obuf;

  while (can_send && ezsession->obuf.len + out->len < GRPC_OBUF_HIGH_WATER) {
    const u8 *data;
    ssize_t n = nghttp2_session_mem_send(ezsession->ngsession, &data);
    if (n < 0) {
      LOG_ERROR(ezsession->server, "nghttp2: %s. killing...\n", nghttp2_strerror((int)n));
      return 1;
    }
    if (n == 0)
      break;
    if (buf_append(out, data, (size_t)n))
      return 1;
  }

  if (ezsession->is_tls) {
    /* one SSL_write for the whole batch keeps the records large */
    if (out->len > 0) {
      ssize_t n = grpc_tls_write(ezsession, out->data, out->len);
      out->len = 0;
      if (n <= 0)
        return 1;
    }
    if (grpc_tls_drain(ezsession, &ezsession->obuf))
      return 1;
  }
  return 0;
}

static void on_session_write(uv_write_t *req, int status) {
  grpc_session *ezsession = req->data;
  ezsession->is_writing = false;
  ezsession->wbuf.len = 0;
  if (!ezsession->is_active || ezsession->is_closing)
    return;
  if (status < 0) {
    LOG_DEBUG(ezsession->server, "write: %s\n", uv_strerror(status));
    session_close(ezsession);
    return;
  }
  session_flush(ezsession);
}

void session_flush(grpc_session *ezsession) {
  /* an in-flight write resumes the flush from on_session_write */
  if (!ezsession->is_active || ezsession->is_closing || ezsession->is_writing)
    return;

  for (;;) {
    if (session_serialize(ezsession)) {
      session_close(ezsession);
      return;
    }
    if (ezsession->obuf.len == 0)
      break;

    uv_buf_t buf = uv_buf_init((char *)ezsession->obuf.data, (unsigned int)ezsession->obuf.len);
    int n = uv_try_write((uv_stream_t *)&ezsession->tcp, &buf, 1);
    if (n == UV_EAGAIN)
      n = 0;
    if (n < 0) {
      LOG_DEBUG(ezsession->server, "write: %s\n", uv_strerror(n));
      session_close(ezsession);
      return;
    }
    if ((size_t)n == ezsession->obuf.len) {
      ezsession->obuf.len = 0;
      continue;
    }

    /* the socket buffer is full. let libuv write the rest while obuf keeps collecting */
    grpc_buf tmp = ezsession->wbuf;
    ezsession->wbuf = ezsession->obuf;
    ezsession->obuf = tmp;
    ezsession->obuf.len = 0;
    buf = uv_buf_init((char *)ezsession->wbuf.data + n,
                      (unsigned int)(ezsession->wbuf.len - (size_t)n));
    ezsession->write_req.data = ezsession;
    if (uv_write(&ezsession->write_req, (uv_stream_t *)&ezsession->tcp, &buf, 1,
                 on_session_write) != 0) {
      session_close(ezsession);
      return;
    }
    ezsession->is_writing = true;
    return;
  }

  if (!nghttp2_session_want_read(ezsession->ngsession) &&
      !nghttp2_session_want_write(ezsession->ngsession))
    session_close(ezsession);
}

grpc_stream *stream_new(i32 stream_id) {
  grpc_stream *ezstream = grpc_calloc(1, sizeof(*ezstream));
  if (ezstream == nullptr)
    return nullptr;
  ezstream->stream_id = stream_id;
  ezstream->recv_data = ezstream->recv_inline;
  ezstream->recv_cap = sizeof(ezstream->recv_inline);
  grpc_list_init(&ezstream->lqueue_omessages);
  return ezstream;
}

void stream_free(grpc_stream *ezstream) {
  if (ezstream == nullptr)
    return;
  if (ezstream->recv_data != ezstream->recv_inline)
    grpc_free(ezstream->recv_data);
  grpc_free(ezstream->pending_trailers);

  for (size_t i = 0; i < ezstream->nb_headers; i++) {
    nghttp2_rcbuf_decref(ezstream->header_rcbufs[i][0]);
    nghttp2_rcbuf_decref(ezstream->header_rcbufs[i][1]);
  }

  grpc_message *msg;
  while ((msg = grpc_list_pop_front(&ezstream->lqueue_omessages)) != nullptr) {
    grpc_message_free(msg);
  }

  grpc_free(ezstream);
}

void stream_link(grpc_session *session, grpc_stream *stream) {
  stream->prev = nullptr;
  stream->next = session->streams;
  if (session->streams != nullptr)
    session->streams->prev = stream;
  session->streams = stream;
  session->nb_streams++;
}

void stream_unlink(grpc_session *session, grpc_stream *stream) {
  if (stream->prev != nullptr)
    stream->prev->next = stream->next;
  else
    session->streams = stream->next;
  if (stream->next != nullptr)
    stream->next->prev = stream->prev;
  stream->prev = stream->next = nullptr;
  session->nb_streams--;
}
