# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "USFS requires an out-of-source build; use: cmake -S . -B build")
endif()

if(DEFINED CMAKE_GENERATOR AND NOT CMAKE_GENERATOR STREQUAL "Unix Makefiles")
    message(FATAL_ERROR "USFS supports only the Unix Makefiles generator")
endif()

if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(USFS_DEFAULT_GCC NAMES gcc REQUIRED)
    set(CMAKE_C_COMPILER "${USFS_DEFAULT_GCC}" CACHE FILEPATH "AIX C compiler" FORCE)
endif()

project(usfs VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "AIX")
    message(FATAL_ERROR "USFS packages must be built directly on AIX")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "USFS currently requires GCC on AIX")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -maix64")
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> -X64 rc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> -X64 r <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH ":")

set(USFS_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "RPM package version")
set(USFS_PACKAGE_RELEASE "1" CACHE STRING "RPM package release")

find_program(USFS_GIT_EXECUTABLE NAMES git)
set(_usfs_revision unknown)

if(USFS_GIT_EXECUTABLE)
    execute_process(
        COMMAND "${USFS_GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _usfs_git_revision_status
        OUTPUT_VARIABLE _usfs_git_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_usfs_git_revision_status EQUAL 0 AND NOT _usfs_git_revision STREQUAL "")
        set(_usfs_revision "${_usfs_git_revision}")
    endif()
endif()

set(USFS_BUILD_REVISION "${_usfs_revision}" CACHE STRING "Source revision recorded in artifacts")
set(USFS_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(USFS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(USFS_PACKAGES_DIR "${CMAKE_BINARY_DIR}/packages")
set(USFS_KERNEX_IMPORT_FILE /lib/kernex.exp)

set(USFS_DIAGNOSTIC_FLAGS
    -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral
    -Wundef -Wpointer-arith -Wwrite-strings -maix64)

set(USFS_COMMON_DEFINITIONS DEBUG)
set(USFS_KERNEL_DEFINITIONS _KERNEL _ALL_SOURCE _POSIX_SOURCE)

function(usfs_set_output target directory name)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${directory}"
        ARCHIVE_OUTPUT_DIRECTORY "${directory}"
        OUTPUT_NAME "${name}")
endfunction()

function(usfs_common_target target)
    target_compile_options(${target} PRIVATE ${USFS_DIAGNOSTIC_FLAGS})
    target_compile_definitions(${target} PRIVATE ${USFS_COMMON_DEFINITIONS})
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/common"
        "${CMAKE_SOURCE_DIR}/src/client/include")
endfunction()

set(USFS_ERR_TEMPLATE "${CMAKE_SOURCE_DIR}/packaging/aix/odm/usfs.err")

add_custom_command(
    OUTPUT "${USFS_GENERATED_DIR}/usfs_errids.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USFS_GENERATED_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${USFS_ERR_TEMPLATE}" "${USFS_GENERATED_DIR}/usfs.err"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${USFS_GENERATED_DIR}"
        errupdate -h -n usfs.err
    COMMAND "${CMAKE_COMMAND}" -E rename
        "${USFS_GENERATED_DIR}/usfs.err.h"
        "${USFS_GENERATED_DIR}/usfs_errids.h"
    DEPENDS "${USFS_ERR_TEMPLATE}"
    VERBATIM)
add_custom_target(_usfs_errids
    DEPENDS "${USFS_GENERATED_DIR}/usfs_errids.h")

set(USFS_KERNEL_CORE_SOURCES
    src/common/usfs_validate.c
    src/common/usfs_status.c
    src/kernel/fs/helpers.c
    src/kernel/fs/io_helpers.c
    src/kernel/device/protocol/request_build.c
    src/kernel/device/protocol/request_state.c
    src/kernel/device/protocol/request_wait.c
    src/kernel/device/runtime_config.c
    src/kernel/device/observability.c
    src/kernel/device/protocol/transport_validation.c
    src/kernel/lifecycle/state.c
    src/kernel/instrumentation/ras.c
    src/kernel/device/protocol/request.c
    src/kernel/fs/fs.c
    src/kernel/lifecycle/config.c)

function(usfs_kernel_objects target)
    add_library(${target} OBJECT EXCLUDE_FROM_ALL ${ARGN})
    target_compile_options(${target} PRIVATE ${USFS_DIAGNOSTIC_FLAGS})
    target_compile_definitions(${target} PRIVATE ${USFS_KERNEL_DEFINITIONS})
    target_include_directories(${target} PRIVATE
        "${USFS_GENERATED_DIR}"
        "${CMAKE_SOURCE_DIR}/src/kernel"
        "${CMAKE_SOURCE_DIR}/src/common")
    add_dependencies(${target} _usfs_errids)
endfunction()

usfs_kernel_objects(_usfs_kernel_release_objects ${USFS_KERNEL_CORE_SOURCES}
    src/kernel/instrumentation/services.c
    src/kernel/instrumentation/adapter.c)
add_library(_usfs_kernel_hooks OBJECT EXCLUDE_FROM_ALL src/kernel/instrumentation/hooks.c)
target_compile_options(_usfs_kernel_hooks PRIVATE ${USFS_DIAGNOSTIC_FLAGS})

add_custom_command(
    OUTPUT "${USFS_BIN_DIR}/usfs.kext"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USFS_BIN_DIR}"
    COMMAND "${CMAKE_LINKER}" -b64 -e usfs_kext_entry
        -o "${USFS_BIN_DIR}/usfs.kext" "-bI:${USFS_KERNEX_IMPORT_FILE}"
        $<TARGET_OBJECTS:_usfs_kernel_release_objects>
        $<TARGET_OBJECTS:_usfs_kernel_hooks> -lsys -lcsys
    DEPENDS _usfs_kernel_release_objects _usfs_kernel_hooks
        $<TARGET_OBJECTS:_usfs_kernel_release_objects>
        $<TARGET_OBJECTS:_usfs_kernel_hooks>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(_usfs_kernel_release DEPENDS "${USFS_BIN_DIR}/usfs.kext")

add_library(_usfs_common_methods OBJECT EXCLUDE_FROM_ALL
    src/common/usfs_common.c src/common/instrumentation/config_normal.c)
usfs_common_target(_usfs_common_methods)

foreach(_method IN ITEMS cfgusfs ucfgusfs chusfs)
    if(_method STREQUAL cfgusfs)
        set(_source src/cli/configure_device.c)
    elseif(_method STREQUAL ucfgusfs)
        set(_source src/cli/unconfigure_device.c)
    else()
        set(_source src/cli/change_device.c)
    endif()
    add_executable(_usfs_${_method} EXCLUDE_FROM_ALL
        "${_source}" $<TARGET_OBJECTS:_usfs_common_methods>)
    usfs_common_target(_usfs_${_method})
    target_link_libraries(_usfs_${_method} PRIVATE cfg odm)
    usfs_set_output(_usfs_${_method} "${USFS_BIN_DIR}" "${_method}")
endforeach()

add_library(_usfs_fuse3 STATIC EXCLUDE_FROM_ALL
    src/client/lib/fuse_opt.c src/client/lib/fuse_log.c src/client/lib/fuse_aix.c src/common/usfs_validate.c)
usfs_common_target(_usfs_fuse3)
target_link_libraries(_usfs_fuse3 PUBLIC pthreads)
usfs_set_output(_usfs_fuse3 "${USFS_BIN_DIR}" fuse3)

function(usfs_add_program target output)
    add_executable(${target} EXCLUDE_FROM_ALL ${ARGN})
    usfs_common_target(${target})
    usfs_set_output(${target} "${USFS_BIN_DIR}" "${output}")
endfunction()

usfs_add_program(_usfs_ctl usfsctl src/common/usfs_status.c src/cli/usfsctl.c)
target_compile_definitions(_usfs_ctl PRIVATE
    USFS_PACKAGE_VERSION="${USFS_PACKAGE_VERSION}"
    USFS_BUILD_REVISION="${USFS_BUILD_REVISION}")
foreach(_example IN ITEMS mirror memfs)
    usfs_add_program(_usfs_${_example} usfs_${_example} "examples/${_example}.c")
    target_link_libraries(_usfs_${_example} PRIVATE _usfs_fuse3 pthreads)
endforeach()

add_custom_target(_usfs_release_artifacts DEPENDS
    _usfs_kernel_release _usfs_cfgusfs _usfs_ucfgusfs _usfs_chusfs
    _usfs_fuse3 _usfs_ctl
    _usfs_mirror _usfs_memfs)

set(USFS_RPM_FILE
    "usfs-${USFS_PACKAGE_VERSION}-${USFS_PACKAGE_RELEASE}.aix${CMAKE_HOST_SYSTEM_VERSION}.ppc.rpm")
set(USFS_RPM_PATH "${USFS_PACKAGES_DIR}/${USFS_RPM_FILE}")
add_custom_target(package ALL
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/build-package.sh"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
        "${USFS_PACKAGE_VERSION}" "${USFS_PACKAGE_RELEASE}"
        "${CMAKE_HOST_SYSTEM_VERSION}"
    BYPRODUCTS "${USFS_RPM_PATH}"
    DEPENDS _usfs_release_artifacts
        "${CMAKE_SOURCE_DIR}/packaging/aix/rpm/usfs.spec"
        "${CMAKE_SOURCE_DIR}/scripts/aix/build-package.sh"
    VERBATIM)
