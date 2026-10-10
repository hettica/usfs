# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
        message(FATAL_ERROR "Use an out-of-source native client build")
    endif()
    if(NOT DEFINED CMAKE_C_COMPILER)
        find_program(USFS_CLIENT_DEFAULT_GCC NAMES gcc REQUIRED)
        set(CMAKE_C_COMPILER "${USFS_CLIENT_DEFAULT_GCC}" CACHE FILEPATH "AIX C compiler" FORCE)
    endif()
    project(usfs_client VERSION 0.1.0 LANGUAGES C)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "AIX" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Build the USFS client directly on AIX")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "The USFS client requires GCC on AIX")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Unix Makefiles")
    message(FATAL_ERROR "The USFS client supports the Unix Makefiles generator")
endif()

set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> -X64 rc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> -X64 r <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH ":")

add_library(_usfs_client_objects OBJECT
    lib/usfs_client.c
    ../common/usfs_validate.c)
add_library(USFS::client_objects ALIAS _usfs_client_objects)
set_target_properties(_usfs_client_objects PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(_usfs_client_objects PUBLIC -maix64 -pthread PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-format-nonliteral
    -Wundef -Wpointer-arith -Wwrite-strings)
target_link_options(_usfs_client_objects INTERFACE -maix64)
target_link_libraries(_usfs_client_objects INTERFACE pthreads)
target_compile_definitions(_usfs_client_objects PRIVATE DEBUG)
target_include_directories(_usfs_client_objects
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/usfs"
        "${CMAKE_CURRENT_SOURCE_DIR}/../common")

add_library(_usfs_client STATIC $<TARGET_OBJECTS:_usfs_client_objects>)
add_library(USFS::client ALIAS _usfs_client)
set_target_properties(_usfs_client PROPERTIES
    OUTPUT_NAME usfs PREFIX "lib" SUFFIX ".a")
target_include_directories(_usfs_client INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_options(_usfs_client INTERFACE -maix64 -pthread)
target_link_options(_usfs_client INTERFACE -maix64)
target_link_libraries(_usfs_client INTERFACE pthreads)
