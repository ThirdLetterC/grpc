# gRPC

**gRPC** is a lightweight, high-performance gRPC server library in C23 for Linux. It is built on [libuv](https://libuv.org/) and [nghttp2](https://nghttp2.org/), and exposes a simple poll-and-handle API: no hidden threads, no code generator, no callback soup. You decide how requests are scheduled, how memory is allocated, and how many cores to use.

---

## Features

- **Poll-based API**: one call (`grpc_server_poll()`) runs the event loop and returns a batch of events (connections, messages, cancellations, deadlines) for you to handle.
- **All 4 RPC modes**: unary, server streaming, client streaming and bidirectional streaming.
- **TLS with ALPN**: HTTP/2 over TLS 1.2 and 1.3, negotiating `h2`. Seven TLS libraries are supported: OpenSSL, LibreSSL, BoringSSL, AWS-LC, wolfSSL, GnuTLS and rustls (see [TLS backends](#tls-backends)).
- **Protobuf-C helpers**: optional [`protobuf-c`](https://github.com/protobuf-c/protobuf-c) integration (`GRPC_PB_UNPACK`, `grpc_pb_create_response`).
- **FlatBuffers helpers**: optional [`flatcc`](https://github.com/dvidelabs/flatcc) integration with verified, zero-copy reads (`GRPC_FB_UNPACK`, `grpc_fb_create_response`).
- **Interceptors**: pre-call hooks run with `grpc_server_intercept_pre()`, post-call hooks with `grpc_server_intercept_post()`. A built-in Bearer token interceptor is available (`grpc_interceptor_create_bearer_auth`).
- **Health checking**: the standard `/grpc.health.v1.Health/Check` service with per-service status.
- **Server reflection**: `grpc.reflection.v1` and `v1alpha` `ServerReflectionInfo`.
- **Metadata**: read request headers, send initial metadata and trailers, and Base64 `*-bin` values.
- **Deadlines**: `grpc-timeout` is parsed. Expired streams are ended with `DEADLINE_EXCEEDED` and reported as `GRPC_EVENT_TIMEOUT`.
- **Message compression**: gzip and deflate through zlib, plus optional Zstandard and Snappy codecs. Select the codec and compression level per message.
- **Rich error model**: `google.rpc.Status` with `Any` details (e.g. `ErrorInfo`) sent in `grpc-status-details-bin`.
- **Pluggable allocators**: libc `malloc`, `mimalloc`, `jemalloc`, or your own allocator set at runtime.

---

## Architecture

You register service paths, then poll for events and answer them:

```c
struct grpc_event {
  union {
    grpc_event_message  message;   // Received gRPC messages on a stream
    grpc_event_dataloss dataloss;  // Truncated message at end of stream
    grpc_event_cancel   cancel;    // Stream cancelled by the client (RST_STREAM)
    grpc_event_timeout  timeout;   // Stream deadline exceeded
  };
  grpc_event_type type;            // GRPC_EVENT_CONNECT, _DISCONNECT, _MESSAGE, ...
  grpc_session_uuid session_uuid;  // Identifies the client connection
  void *userdata;                     // The pointer you registered with the path
};
```

### I/O model

Each `grpc_server` owns one libuv loop, driven by `grpc_server_poll()`:

- Connections are libuv TCP streams (`uv_tcp_t`). Received bytes go straight from a shared read buffer into nghttp2.
- `grpc_server_session_stream_send()`, `..._stream_end()` and similar calls only **queue** frames. At the start of the next `grpc_server_poll()`, everything queued for a connection (headers, data and trailers of all its streams) is written with a single write. If your next poll may be delayed, call `grpc_server_flush()`.
- The TLS library never touches the socket: ciphertext is passed to and from it in memory, so each batch is encrypted with a single write call and every TLS library is driven the same way.

### Using more cores

A libuv loop runs on one thread. To use more cores, create one server per thread on the same port. The listening sockets use `SO_REUSEPORT`, so the kernel spreads incoming connections across the loops. Health and reflection state is shared and thread-safe. The example server does this when started with `THREADS=N`.

A single HTTP/2 connection always stays on one loop, so multiple loops only help when clients open multiple connections.

### Logging

Set the initial threshold and optional output stream in `grpc_config`, then
change the threshold at runtime with `grpc_server_set_log_level()`:

```c
grpc_config cfg = {
    .port = 50051,
    .log_level = GRPC_LOG_WARNING,
    .log_file = stderr,
};
grpc_server *server = grpc_server_new(&cfg);
/* Later, from any thread while the server is alive: */
grpc_server_set_log_level(server, GRPC_LOG_DEBUG);
```

Each server has its own threshold and output stream. `GRPC_LOG_DEFAULT`
selects info; `GRPC_LOG_OFF` disables server logs. Logging uses the
[ulog](https://github.com/thirdLetterC/ulog) backend.

### Performance

Measured on localhost with `ghz` sending unary `SayHello` calls to the Release example server, built with the default libc allocator. The server was pinned to CPUs 0–7 and the client to CPUs 8–15 on an Intel Core i9-9900K (8 cores / 16 threads). Each result is the median of three runs; each run warmed up with 5,000 calls, then sent 300,000 calls and excluded the first 5,000 from the reported rate.

| Setup | Before | After | Change |
|:---|---:|---:|---:|
| 1 loop, 16 connections × 32 concurrent calls | 83.4k req/s | 83.4k req/s | +0.1% |
| 1 loop, 100 connections × 1 concurrent call | 56.2k req/s | 57.9k req/s | +2.9% |
| 8 loops, 64 connections × 32 concurrent calls | 76.9k req/s | 77.5k req/s | +0.9% |

“Before” is the revision immediately before the receive-buffer, request-header, and message-compression optimizations; “After” includes those changes. Both used the same compiler, build options, server/client CPU affinity, payload (`{"name":"benchmark_user"}`), and `ghz` settings. The results are close to run-to-run variation, so treat them as updated reference measurements rather than a clear throughput gain. Results depend on hardware and client capacity; set `--connections` high enough to keep the server busy.

---

## Requirements & Dependencies

- **Platform**: Linux.
- **Compiler**: GCC 11+ or Clang 14+ with C23 support (`-std=c23`, or `-std=c2x` on older compilers).
- **Build system**: CMake 3.20 or newer.
- **Logger dependency**: CMake fetches the pinned [ulog](https://github.com/ThirdLetterC/ulog) source at configure time. For offline or local development, set `FETCHCONTENT_SOURCE_DIR_ULOG` to an existing checkout.
- **Required libraries**:
  - `libuv` (event loop)
  - `libnghttp2` (HTTP/2)
  - `libuuid` (session identifiers)
  - `pthread`
- **Optional libraries** (detected automatically when installed):
  - A TLS library for TLS: OpenSSL by default, or one of the [other backends](#tls-backends)
  - `zlib` for gzip and deflate compression
  - `libzstd` for Zstandard compression
  - `libsnappy` for Snappy compression
  - `protobuf-c` for the Protobuf-C helpers
  - `flatcc` for the FlatBuffers helpers (no distribution package: build it with `scripts/build-flatcc.sh`)
  - `mimalloc` or `jemalloc` as the allocator (selected at build time)

On Debian / Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config libuv1-dev libnghttp2-dev uuid-dev \
  zlib1g-dev libzstd-dev libsnappy-dev libssl-dev libprotobuf-c-dev protobuf-c-compiler \
  libmimalloc-dev libjemalloc-dev   # optional allocators
```

The Python integration tests also need `grpcio`, `grpcio-tools` and `protobuf` (`pip install grpcio grpcio-tools protobuf`).

---

## Building

The sources are laid out as:

| Directory | Contents |
|:---|:---|
| `include/` | All headers. Applications include `grpc.h` |
| `src/` | Library sources (`.c`) |
| `src/tests/` | Unit tests |
| `examples/` | Example servers and the Python integration tests |

Configure and build the static library, shared library and available examples:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run the registered unit and integration tests with:

```bash
ctest --test-dir build --output-on-failure
```

The CMake targets are `grpc_static` and `grpc_shared`. The `grpc::grpc` alias selects the static library and carries its required link dependencies.

### Compression

The previous implementation provided gzip helpers only. Gzip and deflate both use zlib; deflate uses the zlib-wrapped DEFLATE stream used by gRPC. Zstandard (`zstd`) and Snappy (`snappy`) are detected at build time when their development libraries are installed. Set `-DGRPC_ZSTD=OFF` or `-DGRPC_SNAPPY=OFF` to disable either optional codec.

Compression remains explicit and per message. The server sends the corresponding `grpc-encoding` header when the first response message is compressed, and advertises the codecs enabled in that build. For example:

```c
grpc_message *response = grpc_message_new(payload, payload_len);
grpc_message *compressed = grpc_message_compress(
    response, GRPC_COMPRESS_ZSTD, 5);  // Zstandard level 5
grpc_message_free(response);
if (compressed != nullptr)
    grpc_call_send(call, compressed);   // call_send takes ownership
```

Use `GRPC_COMPRESS_GZIP` or `GRPC_COMPRESS_DEFLATE` with zlib levels `0` through `9`; use `GRPC_COMPRESSION_LEVEL_DEFAULT` for the library default. Zstandard accepts levels supported by the installed Zstandard library. Snappy has no level control and requires `GRPC_COMPRESSION_LEVEL_DEFAULT`. The legacy `grpc_message_compress_gzip()` and raw gzip functions remain available.

Inbound decompression is bounded by `grpc_config.max_message_size` (including the
5-byte gRPC message prefix), independently of the compressed size. Excessive
expansion ends the call with `RESOURCE_EXHAUSTED`; malformed compressed input is
rejected without growing buffers repeatedly. Raw-buffer consumers can use
`grpc_decompress_limited(..., max_output)`, which returns `5` on output-limit
exhaustion and leaves the output pointer null. The existing unbounded helpers
remain available for caller-managed trusted buffers.

Zstandard and Snappy are library-specific gRPC encodings. A peer must support the same encoding token to interoperate; standard gRPC implementations commonly support gzip and identity, while support for deflate, Zstandard and Snappy varies.

Add `-lprotobuf-c` when the Protobuf-C helpers are built, `-lflatccrt` when the FlatBuffers helpers are, and the allocator library (`-lmimalloc` or `-ljemalloc`) when one is selected.

### Build options

| CMake setting or command | Description |
|:---|:---|
| `cmake --build build --target check` | Builds and runs registered unit and integration tests |
| `-DGRPC_ALLOCATOR=mimalloc` | Uses `mimalloc`; also accepts `jemalloc` or `default` |
| `-DGRPC_TLS_BACKEND=<backend>` | Selects `AUTO`, `openssl`, `libressl`, `boringssl`, `awslc`, `wolfssl`, `gnutls`, `rustls` or `none` |
| `-DGRPC_TLS_PREFIX=<dir>` | Searches `<dir>` for the selected TLS library |
| `-DGRPC_PROTOBUF_C=OFF` | Disables Protobuf-C helpers; `AUTO` detects them, `ON` requires them |
| `-DGRPC_FLATCC=OFF` | Disables FlatBuffers helpers; `AUTO` detects them, `ON` requires them |
| `-DGRPC_FLATCC_PREFIX=<dir>` | Searches `<dir>/bin`, `include` and `lib` for flatcc |
| `-DGRPC_ZLIB=OFF`, `-DGRPC_ZSTD=OFF`, `-DGRPC_SNAPPY=OFF` | Disable optional codecs; each accepts `AUTO`, `ON` or `OFF` |
| `-DGRPC_DEBUG_LOGS=ON` | Enables debug logging and debug symbols |
| `-DGRPC_TRACE=ON` | Enables trace logging of every HTTP/2 frame |
| `-DGRPC_ASAN=ON` | Builds with AddressSanitizer |
| `-DGRPC_BUILD_INTEGRATION_TESTS=OFF` | Skips Python client integration tests |
| `-DGRPC_BUILD_EXAMPLES=OFF` | Skips example server targets |
| `cmake --build build --target format` | Formats C sources with `clang-format` |
| `cmake --build build --target format-check` | Checks C formatting with `clang-format` |
| `cmake --build build --target clean` | Removes build output |

### TLS backends

Exactly one TLS library is compiled in, selected with `GRPC_TLS_BACKEND`. Use a separate build directory when switching backends. The versions listed are the ones the test suite has been run against.

| `GRPC_TLS_BACKEND` | Library | How to get it | Notes |
|:---|:---|:---|:---|
| `openssl` | OpenSSL 3.0 | `libssl-dev` | Default when installed |
| `libressl` | LibreSSL 4.3 | `scripts/build-tls-backend.sh libressl <dir>` | |
| `boringssl` | BoringSSL 0.20260929.0 | `scripts/build-tls-backend.sh boringssl <dir>` | |
| `awslc` | AWS-LC 5.10 | `scripts/build-tls-backend.sh awslc <dir>` | |
| `wolfssl` | wolfSSL 5.6 | `libwolfssl-dev` | Needs ALPN support (`--enable-alpn`). Without `--enable-aesni`, AES runs in software and bulk transfers are much slower |
| `gnutls` | GnuTLS 3.8 | `libgnutls28-dev` | |
| `rustls` | rustls via [rustls-ffi](https://github.com/rustls/rustls-ffi) 0.15 | `scripts/build-tls-backend.sh rustls <dir>` (needs a Rust toolchain) | Rejects clients that offer ALPN without `h2` |
| `none` | | | TLS functions fail at runtime |

LibreSSL, BoringSSL and AWS-LC install the same `openssl/*.h` headers as OpenSSL, so they are used from their own prefix. `scripts/build-tls-backend.sh` downloads a pinned, checksum-verified release, builds it, and installs it into a directory of your choice:

```bash
scripts/build-tls-backend.sh awslc "$HOME/tls/awslc"
cmake -S . -B build-awslc -DGRPC_TLS_BACKEND=awslc -DGRPC_TLS_PREFIX="$HOME/tls/awslc"
cmake --build build-awslc --parallel
```

The build fails if the headers it finds don't belong to the selected library, and the TLS unit test checks that the library linked in is the one requested. At runtime, `grpc_tls_backend()` and `grpc_tls_backend_version()` report the library in use.

All backends behave the same way: they load a PEM certificate chain and private key, check that the key matches the certificate, allow TLS 1.2 and 1.3 only, and select `h2` with ALPN. A client that offers no ALPN still connects. For settings grpc doesn't expose, `grpc_tls_ctx_native()` returns the library's own context (`SSL_CTX *`, `WOLFSSL_CTX *`, `gnutls_certificate_credentials_t` or `const struct rustls_server_config *`).

---

## Quickstart

### 1. A complete server

```c
#include <stdio.h>
#include <string.h>
#include "grpc.h"

static void handle_message(grpc_server *server, grpc_event *event) {
  /* the request messages; this server ignores their content */
  grpc_message *request;
  while ((request = grpc_messages_pop(event->message.messages)) != nullptr)
    grpc_message_free(request);

  /* a unary call answers once the client has finished sending */
  if (!event->message.end_stream)
    return;

  const char reply[] = "Hello from grpc!";
  grpc_messages *response = grpc_messages_new(nullptr);
  grpc_messages_push(response, grpc_message_new(0, (void *)reply, strlen(reply)));

  /* both calls only queue frames; the next poll writes them */
  grpc_server_session_stream_send(server, event->session_uuid, event->message.stream_id,
                                     response);
  grpc_server_session_stream_end(server, event->session_uuid, event->message.stream_id,
                                    GRPC_GRPC_STATUS_OK);
  grpc_messages_free(response);
}

int main(void) {
  grpc_global_init(0);

  grpc_server *server = grpc_server_new("0.0.0.0", 19009, /* IPv4 */
                                              "::", 19009,      /* IPv6 */
                                              1024,             /* listen backlog */
                                              nullptr,          /* server settings */
                                              nullptr);         /* HTTP/2 settings */
  if (server == nullptr)
    return 1;

  /* optional */
  /* grpc_server_enable_tls(server, "server.crt", "server.key"); */
  grpc_health_register(server);
  grpc_reflection_register(server);

  /* the userdata pointer comes back in every event for this path */
  static int say_hello;
  grpc_server_register_path(server, "/helloworld.Greeter/SayHello", &say_hello, 0, 0);

  grpc_events *events = grpc_events_new(nullptr);
  for (;;) {
    /* writes queued responses, then waits up to 500 ms for new events */
    if (grpc_server_poll(server, events, 500) < 0)
      break;

    /* events come in blocks; read until none are left */
    grpc_event *block;
    size_t count;
    while ((block = grpc_events_read(events, &count)) != nullptr) {
      for (size_t i = 0; i < count; i++) {
        grpc_event *event = &block[i];
        if (grpc_health_handle_event(server, event) ||
            grpc_reflection_handle_event(server, event)) {
          /* answered by the built-in services */
        } else if (event->type == GRPC_EVENT_MESSAGE && event->userdata == &say_hello) {
          handle_message(server, event);
        } else if (event->type == GRPC_EVENT_DATALOSS) {
          grpc_server_session_stream_end(server, event->session_uuid,
                                            event->dataloss.stream_id,
                                            GRPC_GRPC_STATUS_DATA_LOSS);
        }

        /* the application owns the received messages */
        if (event->type == GRPC_EVENT_MESSAGE)
          grpc_messages_free(event->message.messages);
      }
      grpc_free(block);
    }
  }

  grpc_events_free(events);
  grpc_server_free(server);
  grpc_global_cleanup();
  return 0;
}
```

Paths that are not registered are answered with `UNIMPLEMENTED` automatically and produce no event.

### 2. Protobuf-C requests and responses

With `protobuf-c` installed, unpack requests and pack responses directly:

```c
#include "helloworld.pb-c.h"

grpc_message *req_msg = grpc_messages_pop(event->message.messages);
Helloworld__HelloRequest *req = GRPC_PB_UNPACK(
    Helloworld__HelloRequest, &helloworld__hello_request__descriptor, req_msg);
grpc_message_free(req_msg);

Helloworld__HelloReply reply = HELLOWORLD__HELLO_REPLY__INIT;
reply.message = "Hello, world!";
grpc_messages *res_msgs = grpc_pb_create_response((ProtobufCMessage *)&reply);

grpc_server_session_stream_send(server, event->session_uuid, event->message.stream_id, res_msgs);
grpc_server_session_stream_end(server, event->session_uuid, event->message.stream_id,
                                  GRPC_GRPC_STATUS_OK);

grpc_messages_free(res_msgs);
GRPC_PB_FREE_UNPACKED(req);
```

### 3. FlatBuffers requests and responses

With [flatcc](https://github.com/dvidelabs/flatcc), generate C code for a schema with `flatcc -a greeter.fbs`. Requests are verified and read in place, without copying or unpacking:

```c
#include "greeter_builder.h"
#include "greeter_verifier.h"

flatcc_builder_t builder;
grpc_fb_builder_init(&builder); /* once; uses the grpc allocator */

/* verifies the buffer and returns its root table, or nullptr if it is not a
 * valid greeter.HelloRequest. The table points into req_msg: keep it alive */
greeter_HelloRequest_table_t req = GRPC_FB_UNPACK(greeter_HelloRequest, req_msg);
if (req == nullptr) {
  grpc_server_session_stream_end2(server, event->session_uuid, event->message.stream_id,
                                     GRPC_GRPC_STATUS_INVALID_ARGUMENT, nullptr, nullptr, 0);
  return;
}

/* fields absent from the buffer read as nullptr (strings) or their default */
const char *name = greeter_HelloRequest_name(req);
greeter_HelloReply_start_as_root(&builder);
greeter_HelloReply_message_create_str(&builder, name != nullptr ? name : "World");
greeter_HelloReply_end_as_root(&builder);

/* copies the finished buffer into a message and resets the builder for reuse */
grpc_messages *res_msgs = grpc_fb_create_response(&builder);
grpc_server_session_stream_send(server, event->session_uuid, event->message.stream_id, res_msgs);
grpc_server_session_stream_end(server, event->session_uuid, event->message.stream_id,
                                  GRPC_GRPC_STATUS_OK);
grpc_messages_free(res_msgs);
```

FlatBuffers services use the usual gRPC paths (`/greeter.Greeter/SayHello` for `rpc_service Greeter` in `namespace greeter`). flatcc requires `attribute "streaming";` in schemas that mark streaming methods. Message payloads (`grpc_message.data`) are aligned like `malloc()` memory, so buffers can be read where they are received.

### 4. Metadata, trailers and rich errors

Read a request header and end the stream with custom trailers:

```c
size_t len;
const char *agent = grpc_metadata_get(server, event->session_uuid, event->message.stream_id,
                                         "user-agent", &len);

grpc_metadata_entry trailers[] = {
    {.key = "x-custom-reply", .value = "pong", .key_len = 14, .value_len = 4},
};
grpc_server_session_stream_end2(server, event->session_uuid, event->message.stream_id,
                                   GRPC_GRPC_STATUS_OK, nullptr, trailers, 1);
```

Return an error with `google.rpc.Status` details, serialized into `grpc-status-details-bin`:

```c
grpc_status_detail_entry detail = {
    .type_url = "type.googleapis.com/google.rpc.ErrorInfo",
    .value = error_info_bytes, /* a serialized google.rpc.ErrorInfo */
    .value_len = error_info_len,
};

grpc_server_session_stream_end_with_status_details(
    server, event->session_uuid, event->message.stream_id,
    GRPC_GRPC_STATUS_INVALID_ARGUMENT, "Invalid name provided",
    &detail, 1,     /* details */
    nullptr, 0);    /* extra trailers */
```

### 5. Bearer token authentication

```c
static bool validate_token(const char *token, void *user_data) {
  return strcmp(token, "secret") == 0;
}

grpc_interceptor auth = grpc_interceptor_create_bearer_auth(validate_token, nullptr);
grpc_server_add_interceptor(server, &auth);

/* in the event loop, before running a handler: */
if (!grpc_server_intercept_pre(server, event, "SayHello"))
  continue; /* the stream was already ended with UNAUTHENTICATED */
```

---

## Complete Example & Integration Tests

[`examples/python_client_c_server/`](examples/python_client_c_server/) contains a full server and a Python test client:

- **`server.c`**: unary, server streaming, client streaming and bidirectional streaming handlers, trailers, rich errors, a logging interceptor, health checking and reflection. Gzip-compressed requests are decompressed by the library. Set `THREADS=N` to run N event loops on the same port.
- **`client.py`**: the official Python `grpcio` client, checking 9 scenarios over cleartext (`h2c`) and TLS.

[`examples/flatbuffers/`](examples/flatbuffers/) is a Greeter whose messages are FlatBuffers:

- **`server.c`**: unary and server-streaming methods built with flatcc, rejecting requests that fail verification.
- **`client.py`**: a `grpcio` client that encodes and decodes the messages with the official Python `flatbuffers` package, checking 6 scenarios. It needs `pip install flatbuffers`.

Run the tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## Continuous Integration (CI)

Every push and pull request is tested with GitHub Actions ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) on:

- **OS**: Ubuntu 24.04 and Ubuntu 22.04
- **Compilers**: GCC and Clang
- **Allocators**: libc `malloc`, `mimalloc` and `jemalloc`
- **Sanitizers**: AddressSanitizer builds (`GRPC_ASAN=ON`)
- **FlatBuffers**: flatcc built from source, with GCC, Clang and AddressSanitizer
- **TLS backends**: LibreSSL, BoringSSL, AWS-LC, wolfSSL, GnuTLS and rustls, in addition to OpenSSL
