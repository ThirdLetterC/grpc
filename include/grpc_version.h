#pragma once

#include "grpc_defs.h"
#include "grpc_version_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the linked library's version, e.g. "1.0.0". The returned string has
 * static storage duration and must not be modified or freed. Header macros
 * describe the version used at compile time, which may differ from a loaded DLL. */
GRPC_API const char *grpc_version(void);

#ifdef __cplusplus
}
#endif
