#include <stdlib.h>
#include <string.h>
#include "grpc_mem.h"

#if defined(_MSC_VER) && !defined(GRPC_USE_MIMALLOC) && !defined(GRPC_USE_JEMALLOC)
#include <malloc.h>

/* MSVC requires aligned blocks to be freed/reallocated with the matching CRT
 * functions. Use that allocation family for every default allocation. */
static void *win_malloc(size_t size) {
  return _aligned_malloc(size ? size : 1, _Alignof(grpc_max_align_t));
}
static void *win_calloc(size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size)
    return nullptr;
  size_t bytes = count * size;
  void *ptr = win_malloc(bytes);
  if (ptr != nullptr)
    memset(ptr, 0, bytes);
  return ptr;
}
static void *win_realloc(void *ptr, size_t size) {
  return _aligned_realloc(ptr, size, _Alignof(grpc_max_align_t));
}
static void *win_aligned_alloc(size_t alignment, size_t size) {
  return _aligned_malloc(size ? size : 1, alignment);
}
static void win_free(void *ptr) {
  _aligned_free(ptr);
}
#endif

#if defined(GRPC_USE_MIMALLOC)
#include <mimalloc.h>
static const char default_allocator_name[] = "mimalloc";
static const grpc_allocator default_allocator = {mi_malloc, mi_calloc, mi_realloc, mi_free,
                                                    mi_aligned_alloc};
static grpc_allocator current = {mi_malloc, mi_calloc, mi_realloc, mi_free,
                                    mi_aligned_alloc};
#elif defined(GRPC_USE_JEMALLOC)
/* jemalloc replaces the libc functions when linked */
static const char default_allocator_name[] = "jemalloc";
static const grpc_allocator default_allocator = {malloc, calloc, realloc, free, aligned_alloc};
static grpc_allocator current = {malloc, calloc, realloc, free, aligned_alloc};
#elif defined(_MSC_VER)
static const char default_allocator_name[] = "libc";
static const grpc_allocator default_allocator = {win_malloc, win_calloc, win_realloc, win_free,
                                                 win_aligned_alloc};
static grpc_allocator current = {win_malloc, win_calloc, win_realloc, win_free,
                                 win_aligned_alloc};
#else
static const char default_allocator_name[] = "libc";
static const grpc_allocator default_allocator = {malloc, calloc, realloc, free, aligned_alloc};
static grpc_allocator current = {malloc, calloc, realloc, free, aligned_alloc};
#endif

GRPC_API void grpc_set_allocator(const grpc_allocator *allocator) {
  if (allocator == nullptr) {
    current = default_allocator;
    return;
  }
  if (allocator->malloc)
    current.malloc = allocator->malloc;
  if (allocator->calloc)
    current.calloc = allocator->calloc;
  if (allocator->realloc)
    current.realloc = allocator->realloc;
  if (allocator->free)
    current.free = allocator->free;
  if (allocator->aligned_alloc)
    current.aligned_alloc = allocator->aligned_alloc;
}

GRPC_API const grpc_allocator *grpc_get_allocator(void) {
  return &current;
}

GRPC_API const char *grpc_get_allocator_name(void) {
  return default_allocator_name;
}

GRPC_API void *grpc_malloc(size_t size) {
  return current.malloc(size);
}

GRPC_API void *grpc_calloc(size_t nmemb, size_t size) {
  return current.calloc(nmemb, size);
}

GRPC_API void *grpc_realloc(void *ptr, size_t size) {
  return current.realloc(ptr, size);
}

GRPC_API void grpc_free(void *ptr) {
  current.free(ptr);
}

GRPC_API void *grpc_aligned_alloc(size_t alignment, size_t size) {
  return current.aligned_alloc(alignment, size);
}

GRPC_API char *grpc_strndup(const char *s, size_t n) {
  if (s == nullptr)
    return nullptr;
  size_t len = strnlen(s, n);
  char *r = current.malloc(len + 1);
  if (r != nullptr) {
    memcpy(r, s, len);
    r[len] = '\0';
  }
  return r;
}

GRPC_API char *grpc_strdup(const char *s) {
  return s != nullptr ? grpc_strndup(s, strlen(s)) : nullptr;
}
