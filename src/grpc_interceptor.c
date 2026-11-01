#include <stdlib.h>
#include <string.h>
#include "internal/grpc_platform.h"
#include "core.h"
#include "grpc_timeout.h"

GRPC_API grpc_err grpc_server_add_interceptor(grpc_server *server,
                                                       const grpc_interceptor *interceptor) {
  if (server == nullptr || interceptor == nullptr) {
    return GRPC_ERR_INVALID;
  }

  size_t new_nb = server->nb_interceptors + 1;
  grpc_interceptor *new_arr =
      grpc_realloc(server->interceptors, new_nb * sizeof(grpc_interceptor));
  if (new_arr == nullptr) {
    return GRPC_ERR_NOMEM;
  }

  server->interceptors = new_arr;
  server->interceptors[server->nb_interceptors] = *interceptor;
  server->nb_interceptors = new_nb;
  return GRPC_OK;
}

bool interceptors_run_on_call(grpc_call *call) {
  grpc_server *server = call->server;
  for (size_t i = 0; i < server->nb_interceptors; i++) {
    const grpc_interceptor *inc = &server->interceptors[i];
    if (inc->on_call == nullptr)
      continue;
    const char *message = nullptr;
    grpc_status status = inc->on_call(call, &message, inc->userdata);
    if (status != GRPC_STATUS_OK) {
      (void)grpc_call_finish(call, status, message);
      return false;
    }
  }
  return true;
}

void interceptors_run_on_done(grpc_call *call, grpc_status status) {
  /* once, and only for calls that went through on_call */
  if (call->done_reported || !call->started)
    return;
  call->done_reported = true;

  grpc_server *server = call->server;
  uint64_t duration_ms = grpc_time_now_ms() - call->start_ms;
  for (size_t i = 0; i < server->nb_interceptors; i++) {
    const grpc_interceptor *inc = &server->interceptors[i];
    if (inc->on_done != nullptr)
      inc->on_done(call, status, duration_ms, inc->userdata);
  }
}

void interceptors_free(grpc_server *server) {
  for (size_t i = 0; i < server->nb_interceptors; i++) {
    const grpc_interceptor *inc = &server->interceptors[i];
    if (inc->userdata_free != nullptr)
      inc->userdata_free(inc->userdata);
  }
  grpc_free(server->interceptors);
  server->interceptors = nullptr;
  server->nb_interceptors = 0;
}

typedef struct bearer_auth_data {
  grpc_token_validator_fn validator;
  void *userdata;
} bearer_auth_data;

static grpc_status bearer_auth_on_call(grpc_call *call, const char **message,
                                          void *userdata) {
  bearer_auth_data *auth = userdata;
  if (auth == nullptr || auth->validator == nullptr) {
    return GRPC_STATUS_OK;
  }

  size_t len = 0;
  const char *header = grpc_call_metadata(call, "authorization", &len);
  if (header == nullptr) {
    *message = "missing authorization metadata";
    return GRPC_STATUS_UNAUTHENTICATED;
  }
  if (len < 7 || strncasecmp(header, "Bearer ", 7) != 0) {
    *message = "invalid authorization header format (expected 'Bearer <token>')";
    return GRPC_STATUS_UNAUTHENTICATED;
  }

  const char *token = header + 7;
  while (*token == ' ')
    token++;

  if (!auth->validator(token, auth->userdata)) {
    *message = "invalid or expired token";
    return GRPC_STATUS_UNAUTHENTICATED;
  }
  return GRPC_STATUS_OK;
}

GRPC_API grpc_interceptor grpc_interceptor_bearer_auth(grpc_token_validator_fn validator,
                                                                void *userdata) {
  bearer_auth_data *data = grpc_malloc(sizeof(*data));
  if (data != nullptr) {
    data->validator = validator;
    data->userdata = userdata;
  }

  return (grpc_interceptor){
      .name = "bearer_auth",
      .on_call = bearer_auth_on_call,
      .userdata = data,
      .userdata_free = grpc_free,
  };
}
