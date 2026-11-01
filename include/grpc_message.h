#pragma once

#include "grpc_defs.h"

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdalign.h>
#endif

typedef struct grpc_message grpc_message;

typedef enum {
  GRPC_COMPRESS_IDENTITY = 0,
  GRPC_COMPRESS_GZIP = 1,
  GRPC_COMPRESS_DEFLATE = 2,
  GRPC_COMPRESS_ZSTD = 3,
  GRPC_COMPRESS_SNAPPY = 4,
} grpc_compression_algorithm;

/**
 * A gRPC message: one serialized request or response.
 */
struct grpc_message {
  /** Length of ``data`` in bytes. */
  uint32_t len;
  /** 1 if ``data`` is compressed (the gRPC Compressed-Flag), otherwise 0. */
  uint8_t cflag;
  /** Encoding used when cflag is 1; ignored for uncompressed messages. */
  grpc_compression_algorithm compression;
  /** Codec level, or GRPC_COMPRESSION_LEVEL_DEFAULT. */
  int compression_level;
  /** The payload, aligned like ``malloc()`` memory, so formats that are read
   * in place (FlatBuffers) can use it directly. */
  alignas(grpc_max_align_t) uint8_t data[];
};

/**
 * Creates a message holding a copy of ``data``.
 * Returns nullptr when out of memory.
 */
GRPC_API grpc_message *grpc_message_new(const void *data, size_t len);

/**
 * Creates a message with ``len`` uninitialized bytes, for serializing in place.
 * Returns nullptr when out of memory.
 */
GRPC_API grpc_message *grpc_message_alloc(size_t len);

/**
 * Frees a message. Accepts nullptr.
 */
GRPC_API void grpc_message_free(grpc_message *message);
