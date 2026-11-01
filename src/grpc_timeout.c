#include <time.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include "core.h"
#include "grpc_timeout.h"

uint64_t grpc_time_now_ms(void) {
  return uv_hrtime() / 1000000ULL;
}

int64_t grpc_timeout_parse_ms(const char *timeout_str) {
  if (timeout_str == nullptr || *timeout_str == '\0') {
    return -1;
  }

  // Find where numeric value ends
  const char *p = timeout_str;
  while (*p && isdigit((unsigned char)*p)) {
    p++;
  }

  if (p == timeout_str || *p == '\0') {
    return -1; // No digits or missing unit
  }

  char unit = *p;
  // If there are extra characters after the unit
  if (*(p + 1) != '\0') {
    return -1;
  }

  // Parse numeric part
  char *endptr = nullptr;
  unsigned long long val = strtoull(timeout_str, &endptr, 10);
  if (endptr != p || val == 0) {
    return -1;
  }

  uint64_t ms = 0;
  switch (unit) {
  case 'H': // Hour
    if (val > (ULLONG_MAX / (3600ULL * 1000ULL)))
      return -1;
    ms = val * 3600ULL * 1000ULL;
    break;
  case 'M': // Minute
    if (val > (ULLONG_MAX / (60ULL * 1000ULL)))
      return -1;
    ms = val * 60ULL * 1000ULL;
    break;
  case 'S': // Second
    if (val > (ULLONG_MAX / 1000ULL))
      return -1;
    ms = val * 1000ULL;
    break;
  case 'm': // Millisecond
    ms = val;
    break;
  case 'u':                        // Microsecond
    ms = (val + 999ULL) / 1000ULL; // Ceiling to at least 1ms if non-zero
    break;
  case 'n':                              // Nanosecond
    ms = (val + 999999ULL) / 1000000ULL; // Ceiling to at least 1ms if non-zero
    break;
  default:
    return -1;
  }

  if (ms > (uint64_t)INT64_MAX) {
    return INT64_MAX;
  }

  return (int64_t)ms;
}

/* ends the calls past their deadline and returns the earliest deadline left */
static u64 expire_deadlines(grpc_server *server, u64 now_ms) {
  u64 next_deadline_ms = UINT64_MAX;
  for (size_t i = 0; i < server->nb_sessions; i++) {
    grpc_session *session = &server->sessions[i];
    if (!session->is_active || session->is_closing || session->ngsession == nullptr)
      continue;
    grpc_stream *stream = session->streams;
    while (stream != nullptr) {
      /* call_abort() may not free the stream, but read next first anyway */
      grpc_stream *next = stream->next;
      if (stream->is_deadline_set) {
        if (now_ms >= stream->deadline_ms) {
          stream->is_deadline_set = false;
          if (stream->call != nullptr)
            call_abort(stream->call, GRPC_STATUS_DEADLINE_EXCEEDED, nullptr);
        } else if (stream->deadline_ms < next_deadline_ms) {
          next_deadline_ms = stream->deadline_ms;
        }
      }
      stream = next;
    }
  }
  return next_deadline_ms;
}

static void on_deadline_timer(uv_timer_t *timer) {
  grpc_server *server = timer->data;
  server->ready_events++;
  server->next_deadline_ms = UINT64_MAX;
  u64 next = expire_deadlines(server, grpc_time_now_ms());
  if (next != UINT64_MAX)
    server_arm_deadline(server, next);
}

void server_arm_deadline(grpc_server *server, u64 deadline_ms) {
  /* the timer already fires earlier */
  if (deadline_ms >= server->next_deadline_ms)
    return;
  server->next_deadline_ms = deadline_ms;
  u64 now = grpc_time_now_ms();
  uv_timer_start(&server->deadline_timer, on_deadline_timer,
                 deadline_ms > now ? deadline_ms - now : 0, 0);
}
