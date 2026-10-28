#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "grpc_compression.h"
#include "grpc_mem.h"

#if defined(GRPC_ENABLE_ZLIB)
#include <zlib.h>
#endif
#if defined(GRPC_ENABLE_ZSTD)
#include <zstd.h>
#endif
#if defined(GRPC_ENABLE_SNAPPY)
#include <snappy-c.h>
#endif

static int zlib_codec(bool compress, bool gzip, int level, const uint8_t *in, size_t in_len,
                      uint8_t **out, size_t *out_len, size_t max_output) {
#if defined(GRPC_ENABLE_ZLIB)
  if (in_len > UINT_MAX) return 1;
  z_stream stream = {0};
  int window = gzip ? 15 + 16 : 15;
  int rc;
  if (compress) {
    if (level == GRPC_COMPRESSION_LEVEL_DEFAULT) level = Z_DEFAULT_COMPRESSION;
    if (level < Z_DEFAULT_COMPRESSION || level > Z_BEST_COMPRESSION) return 1;
    rc = deflateInit2(&stream, level, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY);
  } else {
    rc = inflateInit2(&stream, window);
  }
  if (rc != Z_OK) return 2;
  size_t cap = compress ? (size_t)deflateBound(&stream, (uLong)in_len) : (in_len < 256 ? 256 : in_len > UINT_MAX / 2 ? UINT_MAX : in_len * 2);
  size_t max_cap = !compress && max_output < UINT_MAX ? max_output + 1 : UINT_MAX;
  if (!compress && cap > max_cap) cap = max_cap;
  if ((compress && cap < in_len) || cap > UINT_MAX) { (compress ? deflateEnd : inflateEnd)(&stream); return 1; }
  uint8_t *buf = grpc_malloc(cap ? cap : 1);
  if (!buf) { (compress ? deflateEnd : inflateEnd)(&stream); return 3; }
  stream.next_in = (Bytef *)in;
  stream.avail_in = (uInt)in_len;
  int status = Z_OK;
  do {
    stream.next_out = buf + stream.total_out;
    stream.avail_out = (uInt)(cap - stream.total_out);
    uLong before_in = stream.total_in, before_out = stream.total_out;
    status = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
    if (!compress && stream.total_out > max_output) { grpc_free(buf); inflateEnd(&stream); return 5; }
    if ((compress && status == Z_STREAM_END) || (!compress && status == Z_STREAM_END)) break;
    if (status != Z_OK || (stream.total_in == before_in && stream.total_out == before_out)) {
      grpc_free(buf); (compress ? deflateEnd : inflateEnd)(&stream); return 4;
    }
    if (stream.total_out == cap) {
      size_t next_cap = cap > SIZE_MAX / 2 ? 0 : cap * 2;
      if (next_cap > max_cap) next_cap = max_cap;
      if (next_cap <= cap || next_cap > UINT_MAX) { grpc_free(buf); (compress ? deflateEnd : inflateEnd)(&stream); return 4; }
      uint8_t *next = grpc_realloc(buf, next_cap);
      if (!next) { grpc_free(buf); (compress ? deflateEnd : inflateEnd)(&stream); return 3; }
      buf = next;
      cap = next_cap;
    }
  } while (true);
  *out_len = (size_t)stream.total_out;
  *out = buf;
  (compress ? deflateEnd : inflateEnd)(&stream);
  return 0;
#else
  (void)compress; (void)gzip; (void)level; (void)in; (void)in_len; (void)out; (void)out_len; (void)max_output;
  return -1;
#endif
}

/* Compress directly into caller-owned storage, used by grpc_message_compress
 * to avoid a temporary codec buffer followed by a payload copy. */
static int compress_into(grpc_compression_algorithm algorithm, int level, const uint8_t *in,
                         size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  if (algorithm == GRPC_COMPRESS_GZIP || algorithm == GRPC_COMPRESS_DEFLATE) {
#if defined(GRPC_ENABLE_ZLIB)
    if (in_len > UINT_MAX || out_cap > UINT_MAX)
      return 1;
    if (level == GRPC_COMPRESSION_LEVEL_DEFAULT)
      level = Z_DEFAULT_COMPRESSION;
    if (level < Z_DEFAULT_COMPRESSION || level > Z_BEST_COMPRESSION)
      return 1;
    z_stream stream = {0};
    int window = algorithm == GRPC_COMPRESS_GZIP ? 15 + 16 : 15;
    if (deflateInit2(&stream, level, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY) != Z_OK)
      return 2;
    stream.next_in = (Bytef *)in;
    stream.avail_in = (uInt)in_len;
    stream.next_out = out;
    stream.avail_out = (uInt)out_cap;
    int status = deflate(&stream, Z_FINISH);
    if (status == Z_STREAM_END)
      *out_len = (size_t)stream.total_out;
    deflateEnd(&stream);
    return status == Z_STREAM_END ? 0 : 4;
#else
    (void)level; (void)in; (void)in_len; (void)out; (void)out_cap; (void)out_len;
    return -1;
#endif
  }
  if (algorithm == GRPC_COMPRESS_ZSTD) {
#if defined(GRPC_ENABLE_ZSTD)
    if (level == GRPC_COMPRESSION_LEVEL_DEFAULT)
      level = ZSTD_CLEVEL_DEFAULT;
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
      return 1;
    size_t n = ZSTD_compress(out, out_cap, in, in_len, level);
    if (ZSTD_isError(n))
      return 4;
    *out_len = n;
    return 0;
#else
    (void)level; (void)in; (void)in_len; (void)out; (void)out_cap; (void)out_len;
    return -1;
#endif
  }
  if (algorithm == GRPC_COMPRESS_SNAPPY) {
#if defined(GRPC_ENABLE_SNAPPY)
    if (level != GRPC_COMPRESSION_LEVEL_DEFAULT)
      return 1;
    size_t n = out_cap;
    if (snappy_compress((const char *)in, in_len, (char *)out, &n) != SNAPPY_OK)
      return 4;
    *out_len = n;
    return 0;
#else
    (void)level; (void)in; (void)in_len; (void)out; (void)out_cap; (void)out_len;
    return -1;
#endif
  }
  return -1;
}

static size_t compression_bound(grpc_compression_algorithm algorithm, int level, size_t len) {
  switch (algorithm) {
  case GRPC_COMPRESS_GZIP:
  case GRPC_COMPRESS_DEFLATE:
#if defined(GRPC_ENABLE_ZLIB)
    if (len > UINT_MAX || level < Z_DEFAULT_COMPRESSION || level > Z_BEST_COMPRESSION)
      return 0;
    {
      z_stream stream = {0};
      int window = algorithm == GRPC_COMPRESS_GZIP ? 15 + 16 : 15;
      if (level == GRPC_COMPRESSION_LEVEL_DEFAULT)
        level = Z_DEFAULT_COMPRESSION;
      if (deflateInit2(&stream, level, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
      uLong bound = deflateBound(&stream, (uLong)len);
      deflateEnd(&stream);
      return bound <= UINT_MAX ? (size_t)bound : 0;
    }
#else
    return 0;
#endif
  case GRPC_COMPRESS_ZSTD:
#if defined(GRPC_ENABLE_ZSTD)
    if (level == GRPC_COMPRESSION_LEVEL_DEFAULT)
      level = ZSTD_CLEVEL_DEFAULT;
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
      return 0;
    return ZSTD_compressBound(len);
#else
    return 0;
#endif
  case GRPC_COMPRESS_SNAPPY:
#if defined(GRPC_ENABLE_SNAPPY)
    return level == GRPC_COMPRESSION_LEVEL_DEFAULT ? snappy_max_compressed_length(len) : 0;
#else
    return 0;
#endif
  default:
    return 0;
  }
}

GRPC_API int grpc_compress(grpc_compression_algorithm algorithm, int level,
                                 const uint8_t *in, size_t in_len, uint8_t **out,
                                 size_t *out_len) {
  if ((!in && in_len) || !out || !out_len) return 1;
  static const uint8_t empty_input = 0;
  if (!in) in = &empty_input;
  *out = nullptr; *out_len = 0;
  switch (algorithm) {
  case GRPC_COMPRESS_GZIP:
  case GRPC_COMPRESS_DEFLATE:
    return zlib_codec(true, algorithm == GRPC_COMPRESS_GZIP, level, in, in_len, out, out_len, SIZE_MAX);
  case GRPC_COMPRESS_ZSTD:
#if defined(GRPC_ENABLE_ZSTD)
  {
    if (level == GRPC_COMPRESSION_LEVEL_DEFAULT) level = ZSTD_CLEVEL_DEFAULT;
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel()) return 1;
    size_t cap = ZSTD_compressBound(in_len);
    uint8_t *buf = grpc_malloc(cap ? cap : 1);
    if (!buf) return 3;
    size_t n = ZSTD_compress(buf, cap, in, in_len, level);
    if (ZSTD_isError(n)) { grpc_free(buf); return 4; }
    *out = buf; *out_len = n; return 0;
  }
#else
    return -1;
#endif
  case GRPC_COMPRESS_SNAPPY:
#if defined(GRPC_ENABLE_SNAPPY)
  {
    if (level != GRPC_COMPRESSION_LEVEL_DEFAULT) return 1;
    size_t cap = snappy_max_compressed_length(in_len);
    uint8_t *buf = grpc_malloc(cap ? cap : 1);
    if (!buf) return 3;
    size_t n = cap;
    if (snappy_compress((const char *)in, in_len, (char *)buf, &n) != SNAPPY_OK) { grpc_free(buf); return 4; }
    *out = buf; *out_len = n; return 0;
  }
#else
    return -1;
#endif
  default: return -1;
  }
}

GRPC_API int grpc_decompress_limited(grpc_compression_algorithm algorithm, const uint8_t *in,
    size_t in_len, uint8_t **out, size_t *out_len, size_t max_output) {
  if ((!in && in_len) || !out || !out_len) return 1;
  static const uint8_t empty_input = 0;
  if (!in) in = &empty_input;
  *out = nullptr; *out_len = 0;
  if (algorithm == GRPC_COMPRESS_GZIP || algorithm == GRPC_COMPRESS_DEFLATE)
    return zlib_codec(false, algorithm == GRPC_COMPRESS_GZIP, 0, in, in_len, out, out_len, max_output);
  if (algorithm == GRPC_COMPRESS_ZSTD) {
#if defined(GRPC_ENABLE_ZSTD)
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
    if (!ctx) return 3;
    size_t max_cap = max_output < SIZE_MAX ? max_output + 1 : SIZE_MAX;
    size_t cap = max_cap < 4096 ? max_cap : 4096, used = 0;
    uint8_t *buf = grpc_malloc(cap);
    if (!buf) { ZSTD_freeDCtx(ctx); return 3; }
    ZSTD_inBuffer input = {in, in_len, 0};
    size_t remaining = 1;
    while (remaining != 0 || input.pos < input.size) {
      if (used == cap) {
        if (cap > SIZE_MAX / 2) { grpc_free(buf); ZSTD_freeDCtx(ctx); return 4; }
        size_t next_cap = cap * 2;
        if (next_cap > max_cap) next_cap = max_cap;
        uint8_t *next = grpc_realloc(buf, next_cap);
        if (!next) { grpc_free(buf); ZSTD_freeDCtx(ctx); return 3; }
        buf = next; cap = next_cap;
      }
      ZSTD_outBuffer output = {buf + used, cap - used, 0};
      size_t before = input.pos;
      remaining = ZSTD_decompressStream(ctx, &output, &input);
      if (ZSTD_isError(remaining) || (before == input.pos && output.pos == 0)) {
        grpc_free(buf); ZSTD_freeDCtx(ctx); return 4;
      }
      used += output.pos;
      if (used > max_output) { grpc_free(buf); ZSTD_freeDCtx(ctx); return 5; }
      if (input.pos == input.size && remaining != 0) {
        grpc_free(buf); ZSTD_freeDCtx(ctx); return 4;
      }
    }
    ZSTD_freeDCtx(ctx);
    *out = buf; *out_len = used; return 0;
#else
    return -1;
#endif
  }
  if (algorithm == GRPC_COMPRESS_SNAPPY) {
#if defined(GRPC_ENABLE_SNAPPY)
    size_t cap;
    if (snappy_uncompressed_length((const char *)in, in_len, &cap) != SNAPPY_OK) return 4;
    if (cap > max_output) return 5;
    uint8_t *buf = grpc_malloc(cap ? cap : 1);
    if (!buf) return 3;
    if (snappy_uncompress((const char *)in, in_len, (char *)buf, &cap) != SNAPPY_OK) { grpc_free(buf); return 4; }
    *out = buf; *out_len = cap; return 0;
#else
    return -1;
#endif
  }
  return -1;
}

GRPC_API int grpc_decompress(grpc_compression_algorithm algorithm, const uint8_t *in,
    size_t in_len, uint8_t **out, size_t *out_len) {
  return grpc_decompress_limited(algorithm, in, in_len, out, out_len, SIZE_MAX);
}

GRPC_API int grpc_compress_gzip(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len) {
  return grpc_compress(GRPC_COMPRESS_GZIP, GRPC_COMPRESSION_LEVEL_DEFAULT, in, len, out, out_len);
}
GRPC_API int grpc_decompress_gzip(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len) {
  return grpc_decompress(GRPC_COMPRESS_GZIP, in, len, out, out_len);
}

GRPC_API grpc_message *grpc_message_compress(const grpc_message *msg,
                                                       grpc_compression_algorithm algorithm, int level) {
  if (!msg) return nullptr;
  size_t bound = compression_bound(algorithm, level, msg->len);
  if (bound == 0 || bound > UINT32_MAX)
    return nullptr;
  grpc_message *result = grpc_message_alloc(bound);
  if (result == nullptr)
    return nullptr;
  size_t len = 0;
  if (compress_into(algorithm, level, msg->data, msg->len, result->data, bound, &len) != 0) {
    grpc_message_free(result);
    return nullptr;
  }
  result->len = (uint32_t)len;
  result->cflag = 1;
  result->compression = algorithm;
  result->compression_level = level;
  return result;
}
GRPC_API grpc_message *grpc_message_decompress(const grpc_message *msg,
                                                        grpc_compression_algorithm algorithm) {
  if (!msg) return nullptr;
  if (!msg->cflag) return message_create(0, msg->data, msg->len);
  uint8_t *data = nullptr; size_t len = 0;
  if (grpc_decompress(algorithm, msg->data, msg->len, &data, &len) != 0) return nullptr;
  grpc_message *result = message_create(0, data, len);
  grpc_free(data);
  return result;
}
GRPC_API grpc_message *grpc_message_compress_gzip(const grpc_message *msg) {
  return grpc_message_compress(msg, GRPC_COMPRESS_GZIP, GRPC_COMPRESSION_LEVEL_DEFAULT);
}
GRPC_API grpc_message *grpc_message_decompress_gzip(const grpc_message *msg) {
  return grpc_message_decompress(msg, GRPC_COMPRESS_GZIP);
}
