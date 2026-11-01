# Preserve timestamps when reconfiguring, so compatibility files do not rebuild
# the logger and every source that includes its header unnecessarily.
function(grpc_write_if_different path content)
  if(EXISTS "${path}")
    file(READ "${path}" previous)
    if(previous STREQUAL content)
      return()
    endif()
  endif()
  file(WRITE "${path}" "${content}")
endfunction()

# ulog is a source-only C23 dependency, so populate it through FetchContent and
# provide the small library target it does not define itself. Pin the revision
# to keep configure-time downloads reproducible.
FetchContent_Declare(ulog
  GIT_REPOSITORY https://github.com/ThirdLetterC/ulog.git
  GIT_TAG 99cf8b6e515eee0353eddd024b354551fd99f2dc
  GIT_PROGRESS TRUE
  SOURCE_SUBDIR grpc-source-only)
# The non-existent SOURCE_SUBDIR populates sources without adding upstream targets.
FetchContent_MakeAvailable(ulog)
set(ULOG_SOURCE "${ulog_SOURCE_DIR}/src/ulog.c")
if(NOT GRPC_HAS_C23_CONSTEXPR OR MSVC)
  # Older C2x compilers cannot use const objects as file-scope array bounds.
  # Restore integer constant expressions for the two ulog arrays that need them.
  file(READ "${ULOG_SOURCE}" ulog_source)
  string(REPLACE "static constexpr size_t level_name_storage_size = 64U;"
                 "enum { level_name_storage_size = 64U };" ulog_source "${ulog_source}")
  string(REPLACE "static constexpr int output_total_num =\n    (1 + ULOG_BUILD_EXTRA_OUTPUTS);  // stdout + extra"
                 "enum { output_total_num = (1 + ULOG_BUILD_EXTRA_OUTPUTS) };" ulog_source "${ulog_source}")
  string(REPLACE "static constexpr int output_total_num = 1;  // Only stdout"
                 "enum { output_total_num = 1 };" ulog_source "${ulog_source}")
  if(MSVC)
    # Adapt the pinned dependency's C23 declarations for MSVC's draft mode.
    string(REGEX REPLACE "static constexpr (ulog_level|ulog_topic_id|size_t|int) ([A-Za-z_][A-Za-z_0-9]*)[ \n]*=[ \n]*([^;]+);"
      "enum { \\2 = \\3 };" ulog_source "${ulog_source}")
    string(REPLACE "[[maybe_unused]] enum" "enum" ulog_source "${ulog_source}")
    file(READ "${ulog_SOURCE_DIR}/include/ulog/ulog.h" ulog_header)
    string(REGEX REPLACE "static constexpr (ulog_level|ulog_topic_id|ulog_output_id) ([A-Za-z_][A-Za-z_0-9]*)[ \n]*=[ \n]*([^;]+);"
      "enum { \\2 = \\3 };" ulog_header "${ulog_header}")
    string(REPLACE "[[maybe_unused]] enum" "enum" ulog_header "${ulog_header}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/ulog/ulog")
    grpc_write_if_different("${CMAKE_CURRENT_BINARY_DIR}/generated/ulog/ulog/ulog.h" "${ulog_header}")
    foreach(pair IN ITEMS
        "buf|print_buffer *" "remaining|size_t" "write_pos|char *"
        "written|int" "tgt|print_target" "ev_copy|ulog_event"
        "unlock_status|ulog_status" "tmp|struct tm *" "current_time|time_t"
        "format|const char *" "i|int" "t|topic_t *" "last|topic_t *"
        "topic_id|ulog_topic_id" "id|ulog_topic_id" "next|topic_t *"
        "name_len|size_t" "name_copy|char *" "append_space|bool"
        "output|ulog_output_id" "is_log_allowed|bool" "ev|ulog_event")
      string(REPLACE "|" ";" parts "${pair}")
      list(GET parts 0 name)
      list(GET parts 1 type)
      string(REGEX REPLACE "auto ${name}([ \n]*=)" "${type} ${name}\\1" ulog_source "${ulog_source}")
    endforeach()
  endif()
  set(ulog_compat_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/ulog")
  file(MAKE_DIRECTORY "${ulog_compat_dir}")
  set(ULOG_SOURCE "${ulog_compat_dir}/ulog.c")
  grpc_write_if_different("${ULOG_SOURCE}" "${ulog_source}")
endif()
add_library(ulog STATIC "${ULOG_SOURCE}")
add_library(ulog::ulog ALIAS ulog)
set_target_properties(ulog PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ulog PRIVATE "${ulog_SOURCE_DIR}/include")
if(MSVC)
  target_include_directories(ulog BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/ulog")
endif()
target_link_libraries(ulog PRIVATE grpc_build_options)
target_compile_options(ulog PRIVATE ${GRPC_WARNING_FLAGS})
if(NOT MSVC)
  target_compile_options(ulog PRIVATE -fvisibility=hidden)
endif()
target_compile_definitions(ulog PRIVATE
  _GNU_SOURCE _FORTIFY_SOURCE=2
  ULOG_BUILD_EXTRA_OUTPUTS=1 ULOG_BUILD_TOPICS_MODE=2 ULOG_BUILD_COLOR=0
  ULOG_BUILD_SOURCE_LOCATION=0 ULOG_BUILD_TIME=0 ULOG_BUILD_WARN_NOT_ENABLED=0)
if(NOT GRPC_HAS_C23_CONSTEXPR)
  target_compile_definitions(ulog PRIVATE constexpr=const)
endif()
# ulog also supports pre-C23 GCC/Clang through __auto_type. Use that spelling
# for compilers that only accept the draft -std=c2x mode.
if(NOT GRPC_HAS_C23)
  # bool is a C23 keyword, but draft C2x compilers still need the header.
  target_compile_options(ulog PRIVATE -include stdbool.h)
  target_compile_definitions(ulog PRIVATE auto=__auto_type)
endif()
if(MSVC)
  target_compile_options(ulog PRIVATE /FIstdbool.h)
endif()
if(NOT GRPC_HAS_NULLPTR)
  target_compile_definitions(ulog PRIVATE "nullptr=((void*)0)")
endif()

