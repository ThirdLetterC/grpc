#include <stdlib.h>
#include <string.h>
#include "internal/grpc_platform.h"
#include "grpc_metadata.h"
#include "grpc_mem.h"
#include "core.h"

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

GRPC_API char *grpc_metadata_encode_bin(const uint8_t *data, size_t in_len, size_t *out_len) {
  if (data == nullptr) {
    return nullptr;
  }

  size_t olen = 4 * ((in_len + 2) / 3);
  char *out = grpc_malloc(olen + 1);
  if (out == nullptr) {
    return nullptr;
  }

  size_t i = 0, j = 0;
  while (i < in_len) {
    size_t rem = in_len - i;
    uint32_t octet_a = (uint8_t)data[i++];
    uint32_t octet_b = (rem > 1) ? (uint8_t)data[i++] : 0;
    uint32_t octet_c = (rem > 2) ? (uint8_t)data[i++] : 0;
    uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

    out[j++] = b64_table[(triple >> 18) & 0x3F];
    out[j++] = b64_table[(triple >> 12) & 0x3F];
    out[j++] = (rem > 1) ? b64_table[(triple >> 6) & 0x3F] : '=';
    out[j++] = (rem > 2) ? b64_table[triple & 0x3F] : '=';
  }
  out[j] = '\0';

  if (out_len != nullptr) {
    *out_len = j;
  }
  return out;
}

static inline int b64_char_val(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

GRPC_API uint8_t *grpc_metadata_decode_bin(const char *base64_str, size_t in_len,
                                                 size_t *out_len) {
  if (base64_str == nullptr) {
    return nullptr;
  }

  // Count valid base64 characters (strip trailing '=')
  while (in_len > 0 && (base64_str[in_len - 1] == '=' || base64_str[in_len - 1] == ' ' ||
                        base64_str[in_len - 1] == '\n')) {
    in_len--;
  }

  if (in_len == 0) {
    uint8_t *empty = grpc_malloc(1);
    if (out_len)
      *out_len = 0;
    return empty;
  }

  size_t max_out = (in_len * 3) / 4 + 4;
  uint8_t *out = grpc_malloc(max_out);
  if (out == nullptr) {
    return nullptr;
  }

  size_t i = 0, j = 0;
  uint32_t buf = 0;
  int bits = 0;

  while (i < in_len) {
    int val = b64_char_val(base64_str[i++]);
    if (val < 0)
      continue;
    buf = (buf << 6) | (uint32_t)val;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[j++] = (uint8_t)((buf >> bits) & 0xFF);
    }
  }

  if (out_len != nullptr) {
    *out_len = j;
  }
  return out;
}
