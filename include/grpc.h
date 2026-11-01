/* gRPC is a gRPC server library in C. */
#pragma once

#include "grpc_defs.h"
#include "grpc_version.h"
#include "grpc_error.h"
#include "grpc_status.h"
#include "grpc_mem.h"
#include "grpc_message.h"
#include "grpc_metadata.h"
#include "grpc_call.h"
#include "grpc_event.h"
#include "grpc_server.h"
#include "grpc_interceptor.h"
#include "grpc_status_details.h"
#include "grpc_health.h"
#include "grpc_reflection.h"
#include "grpc_compression.h"
#include "grpc_tls.h"

#if defined(GRPC_ENABLE_PROTOBUF_C)
#include "grpc_protobuf_c.h"
#endif

#if defined(GRPC_ENABLE_FLATCC)
#include "grpc_flatcc.h"
#endif
