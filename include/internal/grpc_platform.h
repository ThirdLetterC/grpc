#pragma once

#include <errno.h>
#include <string.h>
#include <uv.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#define strncasecmp         _strnicmp
#define strcasecmp          _stricmp
#define grpc_socket_close   closesocket
#define GRPC_INVALID_SOCKET INVALID_SOCKET
static inline const char *grpc_socket_error(void) {
  return uv_strerror(uv_translate_sys_error(WSAGetLastError()));
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#define grpc_socket_close   close
#define GRPC_INVALID_SOCKET (-1)
static inline const char *grpc_socket_error(void) {
  return strerror(errno);
}
#endif
