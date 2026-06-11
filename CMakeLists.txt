cmake_minimum_required(VERSION 3.16)
project(mua VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" OFF)
include(cmake/Warnings.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUAJIT REQUIRED IMPORTED_TARGET luajit)
pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
# >=7.87 for CURL_WRITEFUNC_ERROR and curl_easy_header()
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=7.87)

set(MUA_VERSION_PRERELEASE "-dev")
configure_file(cmake/versiondef.h.in ${CMAKE_BINARY_DIR}/config/auto/versiondef.h)

# Everything except main.c compiles once into an object library that feeds
# both the executable and a shared library for FFI unit tests (default symbol
# visibility keeps internals reachable from test/unit).
set(MUA_SOURCES
    src/mua/api/private/helpers.c
    src/mua/log.c
    src/mua/memory.c)

add_library(mua_obj OBJECT ${MUA_SOURCES})
set_target_properties(mua_obj PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET
                                                                      default)
target_link_libraries(mua_obj PUBLIC mua_flags)
target_include_directories(mua_obj PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/config)

add_executable(mua src/mua/main.c)
target_link_libraries(mua PRIVATE mua_obj)

add_library(mua_testlib SHARED)
target_link_libraries(mua_testlib PRIVATE mua_obj)
set_target_properties(mua_testlib PROPERTIES OUTPUT_NAME mua)
