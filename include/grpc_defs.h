#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MSVC's C headers do not provide max_align_t. Its most aligned scalar is
 * double (also the type used by the MSVC C++ library for max_align_t). */
#if defined(_MSC_VER) && !defined(__cplusplus)
typedef double grpc_max_align_t;
#else
typedef max_align_t grpc_max_align_t;
#endif

#if defined(_MSC_VER) && !defined(__cplusplus)
#ifndef alignas
#define alignas _Alignas
#endif
#ifndef alignof
#define alignof _Alignof
#endif
#endif

#ifndef GRPC_API
#ifdef _WIN32
#define GRPC_API
#else
#define GRPC_API [[gnu::visibility("default")]]
#endif
#endif

/* Provide nullptr for compilers that accept C2x but lack the C23 keyword. */
#if !defined(__cplusplus) && \
    (defined(_MSC_VER) || !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#ifndef nullptr
#define nullptr ((void *)0)
#endif
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif
