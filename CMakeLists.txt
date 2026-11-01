cmake_minimum_required(VERSION 3.20)
project(grpc VERSION 1.0.0 LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(GRPC_IS_TOP_LEVEL ON)
else()
  set(GRPC_IS_TOP_LEVEL OFF)
endif()

if(GRPC_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(GRPC_IS_TOP_LEVEL)
  include(CTest)
  set(_grpc_default_tests "${BUILD_TESTING}")
else()
  set(_grpc_default_tests OFF)
endif()
include(FetchContent)
include(GNUInstallDirs)
find_package(PkgConfig QUIET)
# Keep discovery local to this project without replacing FindPkgConfig's macro.
macro(grpc_pkg_check_modules prefix)
  if(PkgConfig_FOUND)
    pkg_check_modules(${prefix} ${ARGN})
  else()
    set(_grpc_pkg_args ${ARGN})
    if("REQUIRED" IN_LIST _grpc_pkg_args)
      message(FATAL_ERROR "pkg-config is required for ${prefix}; install it or choose another backend")
    endif()
    set(${prefix}_FOUND FALSE)
  endif()
endmacro()
find_package(Threads REQUIRED)

option(GRPC_BUILD_TESTS "Build unit tests" ${_grpc_default_tests})
option(GRPC_BUILD_EXAMPLES "Build example servers" ${GRPC_IS_TOP_LEVEL})
option(GRPC_BUILD_SHARED "Build the shared library alongside the static library" ON)
option(GRPC_BUILD_INTEGRATION_TESTS "Register Python gRPC integration tests" ${_grpc_default_tests})
option(GRPC_BUILD_DEVELOPER_TARGETS "Add source formatting targets" ${GRPC_IS_TOP_LEVEL})
option(GRPC_IPO "Enable interprocedural optimization in optimized configurations" OFF)
option(GRPC_ASAN "Build with AddressSanitizer" OFF)
option(GRPC_DEBUG_LOGS "Compile debug logging support" OFF)
option(GRPC_TRACE "Compile per-frame trace logging" OFF)
set(GRPC_ALLOCATOR "default" CACHE STRING "Allocator: default, mimalloc, or jemalloc")
set_property(CACHE GRPC_ALLOCATOR PROPERTY STRINGS default mimalloc jemalloc)
set(GRPC_TLS_BACKEND "AUTO" CACHE STRING "TLS backend: AUTO, openssl, libressl, boringssl, awslc, wolfssl, gnutls, rustls, or none")
set_property(CACHE GRPC_TLS_BACKEND PROPERTY STRINGS AUTO openssl libressl boringssl awslc wolfssl gnutls rustls none)
set(GRPC_TLS_PREFIX "" CACHE PATH "Prefix containing a non-system TLS backend")
set(GRPC_FLATCC_PREFIX "" CACHE PATH "Prefix containing flatcc, headers, and runtime library")
if(GRPC_BUILD_INTEGRATION_TESTS AND GRPC_BUILD_EXAMPLES)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

foreach(feature IN ITEMS PROTOBUF_C FLATCC ZLIB ZSTD SNAPPY)
  set(GRPC_${feature} "AUTO" CACHE STRING "${feature} support: AUTO, ON, or OFF")
  set_property(CACHE GRPC_${feature} PROPERTY STRINGS AUTO ON OFF)
  string(TOUPPER "${GRPC_${feature}}" feature_value)
  if(NOT feature_value MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "GRPC_${feature} must be AUTO, ON, or OFF")
  endif()
  set(GRPC_${feature} "${feature_value}")
endforeach()

string(TOLOWER "${GRPC_ALLOCATOR}" GRPC_ALLOCATOR)
if(NOT GRPC_ALLOCATOR MATCHES "^(default|mimalloc|jemalloc)$")
  message(FATAL_ERROR "GRPC_ALLOCATOR must be default, mimalloc, or jemalloc")
endif()
string(TOLOWER "${GRPC_TLS_BACKEND}" _tls_backend)
set(_tls_backends auto openssl libressl boringssl awslc wolfssl gnutls rustls none)
if(NOT _tls_backend IN_LIST _tls_backends)
  message(FATAL_ERROR "Unknown GRPC_TLS_BACKEND '${GRPC_TLS_BACKEND}'")
endif()

if(GRPC_BUILD_TESTS OR (GRPC_BUILD_EXAMPLES AND GRPC_BUILD_INTEGRATION_TESTS))
  enable_testing()
endif()

set(_grpc_preferred_prefixes)
if(GRPC_TLS_PREFIX)
  list(APPEND _grpc_preferred_prefixes "${GRPC_TLS_PREFIX}")
endif()
if(GRPC_FLATCC_PREFIX)
  list(APPEND _grpc_preferred_prefixes "${GRPC_FLATCC_PREFIX}")
endif()
if(DEFINED ENV{HOME})
  list(APPEND _grpc_preferred_prefixes "$ENV{HOME}/.local" "$ENV{HOME}/.local/usr")
endif()
if(_grpc_preferred_prefixes)
  list(PREPEND CMAKE_PREFIX_PATH ${_grpc_preferred_prefixes})
endif()

include(cmake/GrpcCompiler.cmake)

include(cmake/GrpcUlog.cmake)

include(cmake/GrpcDependencies.cmake)

set(GRPC_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${GRPC_GENERATED_INCLUDE_DIR}")
configure_file(include/grpc_version_config.h.in
  "${GRPC_GENERATED_INCLUDE_DIR}/grpc_version_config.h" @ONLY)

set(GRPC_SOURCES
  src/grpc_call.c
  src/grpc_compression.c
  src/grpc_health.c
  src/grpc_interceptor.c
  src/grpc_list.c
  src/grpc_mem.c
  src/grpc_message.c
  src/grpc_metadata.c
  src/grpc_reflection.c
  src/grpc_server.c
  src/grpc_status.c
  src/grpc_status_details.c
  src/grpc_timeout.c
  src/grpc_tls.c
  src/grpc_version.c
  src/internal_helpers.c
  src/internal_nghttp2_callbacks.c
)
if(GRPC_TLS_SOURCE)
  list(APPEND GRPC_SOURCES "${GRPC_TLS_SOURCE}")
endif()
if(GRPC_PROTOBUF_C_ENABLED)
  list(APPEND GRPC_SOURCES src/grpc_protobuf_c.c)
endif()
if(GRPC_FLATCC_ENABLED)
  list(APPEND GRPC_SOURCES src/grpc_flatcc.c)
endif()

add_library(grpc_objects OBJECT ${GRPC_SOURCES})
set_target_properties(grpc_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(grpc_objects PUBLIC include "${GRPC_GENERATED_INCLUDE_DIR}"
  PRIVATE "${ulog_SOURCE_DIR}/include")
if(MSVC)
  target_include_directories(grpc_objects BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/ulog")
endif()
target_include_directories(grpc_objects PRIVATE include/internal)
target_compile_options(grpc_objects PRIVATE ${GRPC_WARNING_FLAGS})
target_link_libraries(grpc_objects PRIVATE grpc_build_options)
if(NOT MSVC)
  target_compile_options(grpc_objects PRIVATE -fvisibility=hidden)
endif()
if(NOT GRPC_HAS_C23_CONSTEXPR)
  # Clang 18 accepts -std=c23 but does not yet implement C23 constexpr.
  target_compile_definitions(grpc_objects PRIVATE constexpr=const)
endif()
target_compile_definitions(grpc_objects PUBLIC _GNU_SOURCE _FORTIFY_SOURCE=2)
target_compile_definitions(grpc_objects PRIVATE
  ULOG_BUILD_EXTRA_OUTPUTS=1 ULOG_BUILD_TOPICS_MODE=2 ULOG_BUILD_COLOR=0
  ULOG_BUILD_SOURCE_LOCATION=0 ULOG_BUILD_TIME=0 ULOG_BUILD_WARN_NOT_ENABLED=0)
if(NOT GRPC_HAS_NULLPTR)
  target_compile_definitions(grpc_objects PUBLIC "$<$<COMPILE_LANGUAGE:C>:nullptr=((void*)0)>")
endif()
if(NOT GRPC_HAS_ALIGNOF)
  target_compile_definitions(grpc_objects PUBLIC "$<$<COMPILE_LANGUAGE:C>:alignof=_Alignof>")
endif()
if(GRPC_ALLOCATOR_DEFINE)
  target_compile_definitions(grpc_objects PUBLIC "${GRPC_ALLOCATOR_DEFINE}")
endif()
foreach(feature IN ITEMS ZLIB ZSTD SNAPPY PROTOBUF_C FLATCC)
  if(GRPC_${feature}_ENABLED)
    target_compile_definitions(grpc_objects PUBLIC "GRPC_ENABLE_${feature}")
  endif()
endforeach()
if(GRPC_TLS_ENABLED)
  string(TOUPPER "${_tls_backend}" _tls_backend_macro)
  target_compile_definitions(grpc_objects PUBLIC GRPC_ENABLE_TLS "GRPC_TLS_${_tls_backend_macro}")
endif()
if(GRPC_DEBUG_LOGS)
  target_compile_definitions(grpc_objects PUBLIC GRPC_DEBUG)
  if(MSVC)
    target_compile_options(grpc_objects PRIVATE /Zi /Od)
  else()
    target_compile_options(grpc_objects PRIVATE -ggdb3 -O0)
  endif()
endif()
if(GRPC_TRACE)
  target_compile_definitions(grpc_objects PUBLIC GRPC_TRACE)
endif()
if(GRPC_FLATCC_ENABLED)
  target_include_directories(grpc_objects PUBLIC "${FLATCC_INCLUDE_DIR}")
endif()
if(_tls_backend STREQUAL "rustls")
  target_include_directories(grpc_objects PRIVATE "${RUSTLS_INCLUDE_DIR}")
endif()

add_library(grpc_static STATIC $<TARGET_OBJECTS:grpc_objects>)
set_target_properties(grpc_static PROPERTIES OUTPUT_NAME grpc)
set(GRPC_LIBRARY_TARGETS grpc_static)
if(GRPC_BUILD_SHARED)
  add_library(grpc_shared SHARED $<TARGET_OBJECTS:grpc_objects>)
  set_target_properties(grpc_shared PROPERTIES
    OUTPUT_NAME grpc
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")
  if(WIN32)
    # The DLL import library and the static archive must not both be grpc.lib.
    set_target_properties(grpc_shared PROPERTIES ARCHIVE_OUTPUT_NAME grpc_dll
      WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
  add_library(grpc::shared ALIAS grpc_shared)
  list(APPEND GRPC_LIBRARY_TARGETS grpc_shared)
endif()
add_library(grpc::grpc ALIAS grpc_static)
add_library(grpc::static ALIAS grpc_static)

set(GRPC_LINK_LIBRARIES ulog::ulog "${GRPC_NGHTTP2_TARGET}" ${GRPC_LIBUV_TARGET} Threads::Threads
  ${GRPC_TLS_LIBRARIES} ${GRPC_ALLOCATOR_LIBRARIES})
if(WIN32)
  list(APPEND GRPC_LINK_LIBRARIES ws2_32)
endif()
if(GRPC_ZLIB_ENABLED)
  list(APPEND GRPC_LINK_LIBRARIES "${GRPC_ZLIB_TARGET}")
endif()
if(GRPC_ZSTD_ENABLED)
  list(APPEND GRPC_LINK_LIBRARIES "${GRPC_ZSTD_TARGET}")
endif()
if(GRPC_SNAPPY_ENABLED)
  list(APPEND GRPC_LINK_LIBRARIES "${GRPC_SNAPPY_TARGET}")
endif()
if(GRPC_PROTOBUF_C_ENABLED)
  list(APPEND GRPC_LINK_LIBRARIES "${GRPC_PROTOBUF_C_TARGET}")
endif()
if(GRPC_FLATCC_ENABLED)
  list(APPEND GRPC_LINK_LIBRARIES "${FLATCC_LIBRARY}")
endif()
target_link_libraries(grpc_objects PRIVATE ${GRPC_LINK_LIBRARIES})
foreach(library_target IN LISTS GRPC_LIBRARY_TARGETS)
  target_link_libraries(${library_target} PUBLIC ${GRPC_LINK_LIBRARIES})
  target_include_directories(${library_target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_include_directories(${library_target} PUBLIC $<BUILD_INTERFACE:${GRPC_GENERATED_INCLUDE_DIR}>)
  if(GRPC_FLATCC_ENABLED)
    target_include_directories(${library_target} PUBLIC $<BUILD_INTERFACE:${FLATCC_INCLUDE_DIR}>)
  endif()
  target_link_libraries(${library_target} PUBLIC grpc_build_options)
  target_compile_definitions(${library_target} PUBLIC $<TARGET_PROPERTY:grpc_objects,INTERFACE_COMPILE_DEFINITIONS>)
endforeach()

if(GRPC_ASAN)
  if(MSVC)
    message(FATAL_ERROR "GRPC_ASAN is currently supported only with GCC/Clang")
  endif()
  add_library(grpc_sanitizers INTERFACE)
  target_compile_options(grpc_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(grpc_sanitizers INTERFACE -fsanitize=address)
  foreach(target IN LISTS GRPC_LIBRARY_TARGETS)
    target_link_libraries(${target} PUBLIC grpc_sanitizers)
  endforeach()
  target_link_libraries(grpc_objects PRIVATE grpc_sanitizers)
  target_link_libraries(ulog PRIVATE grpc_sanitizers)
endif()

set(GRPC_TEST_TARGETS)
set(GRPC_EXAMPLE_TARGETS)
if(GRPC_BUILD_TESTS)
  enable_testing()
  function(grpc_add_unit_test name source)
    add_executable(test_${name} "${source}" src/tests/unity/unity.c)
    target_include_directories(test_${name} PRIVATE src/tests/unity include/internal)
    target_link_libraries(test_${name} PRIVATE grpc_static)
    add_test(NAME ${name} COMMAND test_${name})
    list(APPEND GRPC_TEST_TARGETS test_${name})
    set(GRPC_TEST_TARGETS "${GRPC_TEST_TARGETS}" PARENT_SCOPE)
  endfunction()

  foreach(name IN ITEMS api list mem metadata timeout compression health interceptor status_details reflection version)
    grpc_add_unit_test(${name} "src/tests/${name}.c")
  endforeach()

  if(GRPC_BUILD_SHARED)
    add_executable(test_version_shared src/tests/version.c src/tests/unity/unity.c)
    target_include_directories(test_version_shared PRIVATE src/tests/unity)
    target_link_libraries(test_version_shared PRIVATE grpc_shared)
    add_test(NAME version_shared COMMAND test_version_shared)
    list(APPEND GRPC_TEST_TARGETS test_version_shared)
  endif()

  if(WIN32 AND GRPC_BUILD_SHARED)
    # Exercise the DLL import library as well as the default static target.
    add_executable(test_mem_shared src/tests/mem.c src/tests/unity/unity.c)
    target_include_directories(test_mem_shared PRIVATE src/tests/unity)
    target_link_libraries(test_mem_shared PRIVATE grpc_shared)
    add_test(NAME mem_shared COMMAND test_mem_shared)
    list(APPEND GRPC_TEST_TARGETS test_mem_shared)
  endif()

  if(GRPC_PROTOBUF_C_ENABLED)
    set(proto_test_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf-tests")
    file(MAKE_DIRECTORY "${proto_test_dir}")
    add_custom_command(OUTPUT "${proto_test_dir}/test_msg.pb-c.c" "${proto_test_dir}/test_msg.pb-c.h"
      COMMAND "${PROTOC_C_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/src/tests" "--c_out=${proto_test_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/test_msg.proto"
      DEPENDS src/tests/test_msg.proto VERBATIM)
    add_executable(test_pb src/tests/pb.c src/tests/unity/unity.c "${proto_test_dir}/test_msg.pb-c.c")
    target_include_directories(test_pb PRIVATE src/tests/unity "${proto_test_dir}")
    target_link_libraries(test_pb PRIVATE grpc_static)
    add_test(NAME pb COMMAND test_pb)
    list(APPEND GRPC_TEST_TARGETS test_pb)
  endif()

  if(GRPC_FLATCC_ENABLED)
    set(flat_test_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers-tests")
    file(MAKE_DIRECTORY "${flat_test_dir}")
    set(flat_test_outputs)
    foreach(schema IN ITEMS test_msg test_noid)
      foreach(suffix IN ITEMS reader builder verifier)
        list(APPEND flat_test_outputs "${flat_test_dir}/${schema}_${suffix}.h")
      endforeach()
    endforeach()
    add_custom_command(OUTPUT ${flat_test_outputs}
      COMMAND "${FLATCC_EXECUTABLE}" -a "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/test_msg.fbs" "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/test_noid.fbs"
      WORKING_DIRECTORY "${flat_test_dir}"
      DEPENDS src/tests/test_msg.fbs src/tests/test_noid.fbs VERBATIM)
    add_executable(test_fb src/tests/fb.c src/tests/unity/unity.c ${flat_test_outputs})
    target_include_directories(test_fb PRIVATE src/tests/unity "${flat_test_dir}")
    target_link_libraries(test_fb PRIVATE grpc_static)
    add_test(NAME fb COMMAND test_fb)
    list(APPEND GRPC_TEST_TARGETS test_fb)
  endif()

  if(GRPC_TLS_ENABLED)
    # Certificate generation needs a host openssl command. Do not search the
    # selected TLS prefix: AWS-LC's installed CLI can resolve against an
    # incompatible system libssl at runtime.
    find_program(GRPC_HOST_OPENSSL_EXECUTABLE NAMES openssl REQUIRED NO_CMAKE_PATH)
    set(tls_test_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/tls-tests")
    file(MAKE_DIRECTORY "${tls_test_dir}")
    add_custom_command(OUTPUT "${tls_test_dir}/test_cert.pem" "${tls_test_dir}/test_key.pem"
      COMMAND "${GRPC_HOST_OPENSSL_EXECUTABLE}" req -batch -x509 -newkey rsa:2048 -nodes -keyout "${tls_test_dir}/test_key.pem" -out "${tls_test_dir}/test_cert.pem" -days 365 -subj /CN=localhost
      VERBATIM)
    add_custom_target(test_tls_certificates DEPENDS "${tls_test_dir}/test_cert.pem" "${tls_test_dir}/test_key.pem")
    add_executable(test_tls src/tests/tls.c src/tests/unity/unity.c)
    target_include_directories(test_tls PRIVATE src/tests/unity include/internal)
    target_compile_definitions(test_tls PRIVATE "EXPECTED_TLS_BACKEND=\"${_tls_backend}\"")
    target_link_libraries(test_tls PRIVATE grpc_static)
    add_dependencies(test_tls test_tls_certificates)
    add_test(NAME tls COMMAND test_tls)
    set_tests_properties(tls PROPERTIES WORKING_DIRECTORY "${tls_test_dir}")
    list(APPEND GRPC_TEST_TARGETS test_tls)
  endif()

endif()

if(GRPC_BUILD_EXAMPLES)
  add_executable(example_single_threaded examples/single_threaded.c)
  set_target_properties(example_single_threaded PROPERTIES OUTPUT_NAME single_threaded.bin)
  target_link_libraries(example_single_threaded PRIVATE grpc_static)
  add_executable(example_multi_threaded examples/multi_threaded.c)
  set_target_properties(example_multi_threaded PROPERTIES OUTPUT_NAME multi_threaded.bin)
  target_link_libraries(example_multi_threaded PRIVATE grpc_static)
  list(APPEND GRPC_EXAMPLE_TARGETS example_single_threaded example_multi_threaded)

  if(GRPC_PROTOBUF_C_ENABLED)
    set(proto_example_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf-example")
    file(MAKE_DIRECTORY "${proto_example_dir}")
    add_custom_command(OUTPUT "${proto_example_dir}/helloworld.pb-c.c" "${proto_example_dir}/helloworld.pb-c.h"
      COMMAND "${PROTOC_C_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/examples/python_client_c_server" "--c_out=${proto_example_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/examples/python_client_c_server/helloworld.proto"
      DEPENDS examples/python_client_c_server/helloworld.proto VERBATIM)
    set(proto_python_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf-python")
    file(MAKE_DIRECTORY "${proto_python_dir}")
    if(GRPC_BUILD_INTEGRATION_TESTS)
      set(proto_python_outputs "${proto_python_dir}/helloworld_pb2.py" "${proto_python_dir}/helloworld_pb2_grpc.py")
      add_custom_command(OUTPUT ${proto_python_outputs}
        COMMAND "${Python3_EXECUTABLE}" -m grpc_tools.protoc
          -I "${CMAKE_CURRENT_SOURCE_DIR}/examples/python_client_c_server"
          "--python_out=${proto_python_dir}" "--grpc_python_out=${proto_python_dir}"
          "${CMAKE_CURRENT_SOURCE_DIR}/examples/python_client_c_server/helloworld.proto"
        DEPENDS examples/python_client_c_server/helloworld.proto VERBATIM)
      add_custom_target(example_python_proto DEPENDS ${proto_python_outputs})
    endif()
    add_executable(example_grpc_server examples/python_client_c_server/server.c "${proto_example_dir}/helloworld.pb-c.c")
    set_target_properties(example_grpc_server PROPERTIES OUTPUT_NAME grpc_server.bin)
    target_include_directories(example_grpc_server PRIVATE "${proto_example_dir}")
    target_link_libraries(example_grpc_server PRIVATE grpc_static)
    list(APPEND GRPC_EXAMPLE_TARGETS example_grpc_server)

    if(GRPC_TLS_ENABLED AND GRPC_BUILD_INTEGRATION_TESTS)
      find_program(GRPC_HOST_OPENSSL_EXECUTABLE NAMES openssl REQUIRED NO_CMAKE_PATH)
      add_custom_command(OUTPUT "${proto_example_dir}/server.crt" "${proto_example_dir}/server.key"
        COMMAND "${GRPC_HOST_OPENSSL_EXECUTABLE}" req -batch -x509 -newkey rsa:2048 -nodes -keyout "${proto_example_dir}/server.key" -out "${proto_example_dir}/server.crt" -days 365 -subj /CN=localhost
        VERBATIM)
      add_custom_target(example_tls_certificates DEPENDS "${proto_example_dir}/server.crt" "${proto_example_dir}/server.key")
      add_dependencies(example_grpc_server example_tls_certificates)
    endif()

    if(GRPC_BUILD_INTEGRATION_TESTS)
      add_dependencies(example_grpc_server example_python_proto)
      add_test(NAME integration_grpc_cleartext
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-integration-test.py"
          --server "$<TARGET_FILE:example_grpc_server>" --client "${CMAKE_CURRENT_SOURCE_DIR}/examples/python_client_c_server/client.py"
          --python-path "${proto_python_dir}" --port 19009 --cwd "${proto_example_dir}")
      if(GRPC_TLS_ENABLED)
        add_test(NAME integration_grpc_tls
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-integration-test.py"
            --server "$<TARGET_FILE:example_grpc_server>" --client "${CMAKE_CURRENT_SOURCE_DIR}/examples/python_client_c_server/client.py"
            --python-path "${proto_python_dir}" --port 19019 --cwd "${proto_example_dir}" --tls)
        set_tests_properties(integration_grpc_tls PROPERTIES DEPENDS integration_grpc_cleartext)
      endif()
    endif()
  endif()

  if(GRPC_FLATCC_ENABLED)
    set(flat_example_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers-example")
    file(MAKE_DIRECTORY "${flat_example_dir}")
    set(flat_example_outputs)
    foreach(suffix IN ITEMS reader builder verifier)
      list(APPEND flat_example_outputs "${flat_example_dir}/greeter_${suffix}.h")
    endforeach()
    add_custom_command(OUTPUT ${flat_example_outputs}
      COMMAND "${FLATCC_EXECUTABLE}" -a "${CMAKE_CURRENT_SOURCE_DIR}/examples/flatbuffers/greeter.fbs"
      WORKING_DIRECTORY "${flat_example_dir}"
      DEPENDS examples/flatbuffers/greeter.fbs VERBATIM)
    add_executable(example_flatbuffers_server examples/flatbuffers/server.c ${flat_example_outputs})
    set_target_properties(example_flatbuffers_server PROPERTIES OUTPUT_NAME flatbuffers_server.bin)
    target_include_directories(example_flatbuffers_server PRIVATE "${flat_example_dir}")
    target_link_libraries(example_flatbuffers_server PRIVATE grpc_static)
    list(APPEND GRPC_EXAMPLE_TARGETS example_flatbuffers_server)
    if(GRPC_BUILD_INTEGRATION_TESTS)
      add_test(NAME integration_flatbuffers
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-integration-test.py"
          --server "$<TARGET_FILE:example_flatbuffers_server>" --client "${CMAKE_CURRENT_SOURCE_DIR}/examples/flatbuffers/client.py"
          --python-path "${CMAKE_CURRENT_SOURCE_DIR}/examples/flatbuffers" --port 19029 --cwd "${flat_example_dir}")
    endif()
  endif()

  if(GRPC_BUILD_INTEGRATION_TESTS)
    add_test(NAME integration_single_threaded
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-integration-test.py"
        --server "$<TARGET_FILE:example_single_threaded>" --client "${CMAKE_CURRENT_SOURCE_DIR}/examples/test_examples.py"
        --client-arg single --port 19041 --cwd "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME integration_multi_threaded
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-integration-test.py"
        --server "$<TARGET_FILE:example_multi_threaded>" --client "${CMAKE_CURRENT_SOURCE_DIR}/examples/test_examples.py"
        --client-arg multi --port 19042 --cwd "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endif()

if(GRPC_BUILD_TESTS OR (GRPC_BUILD_EXAMPLES AND GRPC_BUILD_INTEGRATION_TESTS))
  get_property(grpc_registered_tests DIRECTORY PROPERTY TESTS)
  foreach(test IN LISTS grpc_registered_tests)
    if(test MATCHES "^integration_")
      set_tests_properties(${test} PROPERTIES LABELS integration TIMEOUT 120)
    else()
      set_tests_properties(${test} PROPERTIES LABELS unit TIMEOUT 30)
    endif()
  endforeach()
  add_custom_target(grpc_check
    COMMAND "${CMAKE_CTEST_COMMAND}" -C "$<CONFIG>" --output-on-failure
    DEPENDS ${GRPC_TEST_TARGETS} ${GRPC_EXAMPLE_TARGETS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    USES_TERMINAL VERBATIM)
  if(GRPC_IS_TOP_LEVEL)
    add_custom_target(check DEPENDS grpc_check)
  endif()
endif()

foreach(target IN ITEMS ulog grpc_objects ${GRPC_LIBRARY_TARGETS} ${GRPC_TEST_TARGETS} ${GRPC_EXAMPLE_TARGETS})
  grpc_enable_ipo(${target})
endforeach()

if(GRPC_BUILD_DEVELOPER_TARGETS)
  file(GLOB_RECURSE GRPC_FORMAT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c")
  list(FILTER GRPC_FORMAT_FILES EXCLUDE REGEX "/src/tests/unity/")
  list(FILTER GRPC_FORMAT_FILES EXCLUDE REGEX "_(reader|builder|verifier)\\.h$")
  find_program(CLANG_FORMAT_EXECUTABLE clang-format)
  if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${GRPC_FORMAT_FILES} VERBATIM)
    add_custom_target(format-check COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${GRPC_FORMAT_FILES} VERBATIM)
  else()
    add_custom_target(format COMMAND "${CMAKE_COMMAND}" -E echo "Install clang-format to use this target" COMMAND "${CMAKE_COMMAND}" -E false)
    add_custom_target(format-check COMMAND "${CMAKE_COMMAND}" -E echo "Install clang-format to use this target" COMMAND "${CMAKE_COMMAND}" -E false)
  endif()
endif()

message(STATUS "grpc TLS backend: ${_tls_backend}")
message(STATUS "grpc IPO: ${GRPC_IPO}")
message(STATUS "grpc optional features: zlib=${GRPC_ZLIB_ENABLED}, zstd=${GRPC_ZSTD_ENABLED}, snappy=${GRPC_SNAPPY_ENABLED}, protobuf-c=${GRPC_PROTOBUF_C_ENABLED}, flatcc=${GRPC_FLATCC_ENABLED}")
