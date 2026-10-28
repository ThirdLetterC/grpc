#pragma once

#include "grpc_defs.h"
#include "grpc_message.h"

#define GRPC_COMPRESSION_LEVEL_DEFAULT (-1)

/** Compresses using the selected codec and level; output is freed with grpc_free(). */
GRPC_API int grpc_compress(grpc_compression_algorithm algorithm, int level,
                                 const uint8_t *in, size_t in_len, uint8_t **out,
                                 size_t *out_len);
/** Decompresses using the selected codec; output is freed with grpc_free(). */
GRPC_API int grpc_decompress(grpc_compression_algorithm algorithm,
                                   const uint8_t *in, size_t in_len, uint8_t **out,
                                   size_t *out_len);
/** Bounded decoding with any supported codec. Returns 5 when output exceeds max_output.
 * On failure, *out is nullptr and *out_len is zero. */
GRPC_API int grpc_decompress_limited(grpc_compression_algorithm algorithm,
    const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len, size_t max_output);
/** Returns a compressed copy of msg using algorithm and level. */
GRPC_API grpc_message *grpc_message_compress(const grpc_message *msg,
                                                       grpc_compression_algorithm algorithm,
                                                       int level);
/** Decompresses msg using algorithm; uncompressed messages are duplicated. */
GRPC_API grpc_message *grpc_message_decompress(
    const grpc_message *msg, grpc_compression_algorithm algorithm);

/**
 * Compresses data using gzip (RFC 1952).
 * Allocates ``*out`` with grpc_malloc(). The caller frees it with grpc_free().
 * Returns 0 on success, non-zero on failure.
 */
GRPC_API int grpc_compress_gzip(const uint8_t *in, size_t in_len, uint8_t **out,
                                      size_t *out_len);

/**
 * Decompresses gzip data (RFC 1952).
 * Allocates ``*out`` with grpc_malloc(). The caller frees it with grpc_free().
 * Returns 0 on success, non-zero on failure.
 */
GRPC_API int grpc_decompress_gzip(const uint8_t *in, size_t in_len, uint8_t **out,
                                        size_t *out_len);

/**
 * Returns a new grpc_message with gzip-compressed payload and cflag = 1.
 * Caller frees with grpc_message_free.
 * Returns nullptr on failure.
 */
GRPC_API grpc_message *grpc_message_compress_gzip(const grpc_message *msg);

/**
 * Decompresses a message (if msg->cflag == 1) using gzip and returns a new
 * grpc_message with uncompressed payload and cflag = 0.
 * If msg->cflag == 0, returns a duplicate of msg.
 * Caller frees with grpc_message_free.
 * Returns nullptr on decompression failure.
 */
GRPC_API grpc_message *grpc_message_decompress_gzip(const grpc_message *msg);
