include(CheckCCompilerFlag)
include(CheckCSourceCompiles)

if(MSVC)
  if(MSVC_VERSION LESS 1944)
    message(FATAL_ERROR "MSVC 19.44 (Visual Studio 2022 17.14) or newer is required")
  endif()
  set(GRPC_C_STANDARD_FLAG /std:clatest)
  set(GRPC_HAS_C23 TRUE)
  set(GRPC_WARNING_FLAGS /W4)
else()
  set(GRPC_WARNING_FLAGS -Wall -pedantic)
  check_c_compiler_flag("-std=c23" GRPC_HAS_C23)
  if(GRPC_HAS_C23)
    set(GRPC_C_STANDARD_FLAG -std=c23)
  else()
    check_c_compiler_flag("-std=c2x" GRPC_HAS_C2X)
    if(NOT GRPC_HAS_C2X)
      message(FATAL_ERROR "A compiler with C23 or C2x support is required")
    endif()
    set(GRPC_C_STANDARD_FLAG -std=c2x)
  endif()
endif()
include(CMakePushCheckState)
cmake_push_check_state()
string(APPEND CMAKE_REQUIRED_FLAGS " ${GRPC_C_STANDARD_FLAG}")
check_c_source_compiles("int main(void) { void *p = nullptr; (void)p; return 0; }" GRPC_HAS_NULLPTR)
check_c_source_compiles("typedef struct { int x; } sample; int main(void) { return (int)alignof(sample); }" GRPC_HAS_ALIGNOF)
check_c_source_compiles("static constexpr int value = 1; int main(void) { return value - 1; }"
                        GRPC_HAS_C23_CONSTEXPR)
cmake_pop_check_state()

# Requirements follow grpc targets rather than modifying the caller's directory.
add_library(grpc_build_options INTERFACE)
target_compile_options(grpc_build_options INTERFACE
  "$<$<COMPILE_LANGUAGE:C>:${GRPC_C_STANDARD_FLAG}>")
if(MSVC)
  target_compile_options(grpc_build_options INTERFACE
    "$<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics;/Zc:preprocessor>")
  target_compile_definitions(grpc_build_options INTERFACE
    _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(GRPC_IPO)
  if(MSVC AND GRPC_BUILD_SHARED)
    message(FATAL_ERROR
      "MSVC IPO requires GRPC_BUILD_SHARED=OFF: automatic DLL exports cannot read /GL objects")
  endif()
  include(CheckIPOSupported)
  check_ipo_supported(RESULT grpc_ipo_supported OUTPUT grpc_ipo_error LANGUAGES C)
  if(NOT grpc_ipo_supported)
    message(FATAL_ERROR "GRPC_IPO is unavailable: ${grpc_ipo_error}")
  endif()
endif()

function(grpc_enable_ipo target)
  if(GRPC_IPO)
    set_target_properties(${target} PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
      INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
      INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
  endif()
endfunction()
