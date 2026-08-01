cmake_minimum_required(VERSION 3.24)

project(audWASM LANGUAGES CXX C)

if (DEFINED EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR CMAKE_CXX_COMPILER MATCHES "em\\+\\+")
    set(AUD_TARGETING_WASM ON)
else()
    set(AUD_TARGETING_WASM OFF)
endif()

# ---------------------------------------------------------------------------------------------
# Options (see documentation/tasks/M01-project-infrastructure.md)
# ---------------------------------------------------------------------------------------------
if (AUD_TARGETING_WASM)
    set(AUD_TARGETING_WASM_OFF_DEFAULT OFF)
else()
    set(AUD_TARGETING_WASM_OFF_DEFAULT ON)
endif()

option(AUD_BUILD_TESTS       "Build the Catch2 unit test suite"        ${AUD_TARGETING_WASM_OFF_DEFAULT})
option(AUD_BUILD_CLI         "Build the native aud_cli driver tool"    ${AUD_TARGETING_WASM_OFF_DEFAULT})
option(AUD_BUILD_WASM        "Build the Embind aud_wasm module"        ${AUD_TARGETING_WASM})
option(AUD_ENABLE_SIMD       "Enable -msimd128 (wasm) / -mavx2 or NEON (native)" ON)
option(AUD_ENABLE_ASAN       "Enable ASan+UBSan (native only)"         OFF)
option(AUD_ENABLE_PROFILING  "Enable instrumentation hooks (M22)"      OFF)
option(AUD_WERROR            "Treat warnings as errors"                ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------------------------
# aud_project_options — the ONLY place warnings/standard flags are applied. Never global
# add_compile_options: that would leak into FetchContent'd third-party targets and fail -Werror
# there. See M01 decision.
# ---------------------------------------------------------------------------------------------
add_library(aud_project_options INTERFACE)

target_compile_features(aud_project_options INTERFACE cxx_std_20)

if (MSVC)
    target_compile_options(aud_project_options INTERFACE /W4 /permissive-)
    if (AUD_WERROR)
        target_compile_options(aud_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(aud_project_options INTERFACE -Wall -Wextra -Wpedantic)
    if (AUD_WERROR)
        target_compile_options(aud_project_options INTERFACE -Werror)
    endif()
endif()

if (AUD_ENABLE_SIMD)
    if (AUD_TARGETING_WASM)
        target_compile_options(aud_project_options INTERFACE -msimd128)
    elseif (NOT MSVC)
        target_compile_options(aud_project_options INTERFACE -mavx2)
    endif()
endif()

if (AUD_ENABLE_ASAN AND NOT AUD_TARGETING_WASM AND NOT MSVC)
    target_compile_options(aud_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(aud_project_options INTERFACE -fsanitize=address,undefined)
endif()

if (AUD_ENABLE_PROFILING)
    target_compile_definitions(aud_project_options INTERFACE AUD_ENABLE_PROFILING=1)
endif()

add_library(aud::project_options ALIAS aud_project_options)

# ---------------------------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------------------------
add_subdirectory(third_party)
add_subdirectory(engine)

if (AUD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (AUD_BUILD_CLI)
    add_subdirectory(tools/cli)
endif()

if (AUD_BUILD_WASM)
    add_subdirectory(bindings/wasm)
endif()
