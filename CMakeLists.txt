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
    message(FATAL_ERROR "USFS builds and tests must run directly on AIX")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "USFS currently requires GCC on AIX")
endif()

if(NOT CMAKE_SYSTEM_VERSION MATCHES "^7\\.[23]$")
    message(FATAL_ERROR "USFS packages support AIX 7.2 and 7.3; detected ${CMAKE_SYSTEM_VERSION}")
endif()

set(USFS_PACKAGE_OS "aix${CMAKE_SYSTEM_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -maix64")
set(CMAKE_C_ARCHIVE_CREATE
    "<CMAKE_AR> -X64 rc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND
    "<CMAKE_AR> -X64 r <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH ":")

set(USFS_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "RPM package version")
set(USFS_PACKAGE_RELEASE "1" CACHE STRING "RPM package release")
set(USFS_PACKAGE_PRE_FAULT "none" CACHE STRING "RPM pre-install fault injection")
set(USFS_PACKAGE_EXTRA_REQUIRES "none" CACHE STRING "Additional RPM prerequisite")

find_program(USFS_GIT_EXECUTABLE NAMES git)
set(_usfs_revision unknown)
set(_usfs_dirty unknown)
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
        execute_process(
            COMMAND "${USFS_GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _usfs_git_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_usfs_git_status STREQUAL "")
            set(_usfs_dirty no)
        else()
            set(_usfs_dirty yes)
        endif()
    endif()
endif()
set(USFS_BUILD_REVISION "${_usfs_revision}" CACHE STRING "Source revision recorded in artifacts")
set(USFS_BUILD_DIRTY "${_usfs_dirty}" CACHE STRING "Source worktree state recorded in artifacts")
set(USFS_SOAK_REVISION "${USFS_BUILD_REVISION}" CACHE STRING "Revision used in the soak archive name")

set(USFS_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(USFS_TEST_BIN_DIR "${CMAKE_BINARY_DIR}/testing/bin")
set(USFS_COVERAGE_BIN_DIR "${CMAKE_BINARY_DIR}/coverage/bin")
set(USFS_COVERAGE_RUN quick CACHE STRING "Coverage run identifier")
set(USFS_GCOV_FLAGS -fprofile-arcs -ftest-coverage -fprofile-update=atomic)
set(USFS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(USFS_PACKAGES_DIR "${CMAKE_BINARY_DIR}/packages")
if(CMAKE_CROSSCOMPILING)
    set(USFS_KERNEX_IMPORT_FILE "${CMAKE_SYSROOT}/usr/lib/kernex.exp")
    if(NOT CMAKE_SYSROOT OR NOT EXISTS "${USFS_KERNEX_IMPORT_FILE}")
        message(FATAL_ERROR
            "The AIX cross sysroot does not contain usr/lib/kernex.exp: "
            "${USFS_KERNEX_IMPORT_FILE}")
    endif()
else()
    set(USFS_KERNEX_IMPORT_FILE /lib/kernex.exp)
endif()

set(USFS_DIAGNOSTIC_FLAGS
    -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-format-nonliteral
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
    target_compile_options(${target} PRIVATE ${USFS_DIAGNOSTIC_FLAGS} -pthread)
    target_compile_definitions(${target} PRIVATE ${USFS_COMMON_DEFINITIONS})
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/common")
endfunction()

set(USFS_ERR_TEMPLATE "${CMAKE_SOURCE_DIR}/packaging/aix/odm/usfs.err")
set(USFS_ERRIDS_FALLBACK "${CMAKE_SOURCE_DIR}/src/common/usfs_errids.h")
if(CMAKE_CROSSCOMPILING)
    set(USFS_ERR_TEMPLATE_SHA256
        17e0919f422a5b6c0223433fc652d2468ca0427e7ec3b1ac60131cb28f85a95d)
    set(USFS_ERRIDS_FALLBACK_SHA256
        318515e482faf298b9686b6390ab06de4182c6ba8b97d9325f20b6426610fdae)
    add_custom_target(_usfs_errids
        COMMAND "${CMAKE_COMMAND}"
            -DUSFS_FILE=${USFS_ERR_TEMPLATE}
            -DUSFS_EXPECTED_SHA256=${USFS_ERR_TEMPLATE_SHA256}
            "-DUSFS_FILE_DESCRIPTION=error template"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyPinnedFile.cmake"
        COMMAND "${CMAKE_COMMAND}"
            -DUSFS_FILE=${USFS_ERRIDS_FALLBACK}
            -DUSFS_EXPECTED_SHA256=${USFS_ERRIDS_FALLBACK_SHA256}
            "-DUSFS_FILE_DESCRIPTION=generated error-ID header"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyPinnedFile.cmake"
        VERBATIM)
else()
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
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/instrumentation-boundary.stamp"
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/check-instrumentation-boundary.sh"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/instrumentation-boundary.stamp"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/scripts/aix/check-instrumentation-boundary.sh"
        "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/adapter.c"
        "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/hooks.c"
    VERBATIM)
add_custom_target(_usfs_instrumentation_boundary
    DEPENDS "${CMAKE_BINARY_DIR}/instrumentation-boundary.stamp")

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
    target_compile_definitions(${target} PRIVATE ${USFS_KERNEL_DEFINITIONS} USFS_BUILD_REVISION="${USFS_BUILD_REVISION}")
    target_include_directories(${target} PRIVATE
        "${USFS_GENERATED_DIR}"
        "${CMAKE_SOURCE_DIR}/src/kernel"
        "${CMAKE_SOURCE_DIR}/src/common")
    add_dependencies(${target} _usfs_errids _usfs_instrumentation_boundary)
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
        -o "${USFS_BIN_DIR}/usfs.kext" "-bI:${USFS_KERNEX_IMPORT_FILE}" "-bI:${CMAKE_SOURCE_DIR}/src/kernel/proxy/proxy.exp"
        $<TARGET_OBJECTS:_usfs_kernel_release_objects>
        $<TARGET_OBJECTS:_usfs_kernel_hooks> -lsys -lcsys
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/verify-kernel.sh"
        release "${USFS_BIN_DIR}/usfs.kext"
    DEPENDS _usfs_kernel_release_objects _usfs_kernel_hooks
        $<TARGET_OBJECTS:_usfs_kernel_release_objects>
        $<TARGET_OBJECTS:_usfs_kernel_hooks>
        "${CMAKE_SOURCE_DIR}/scripts/aix/verify-kernel.sh"
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(_usfs_kernel_release DEPENDS "${USFS_BIN_DIR}/usfs.kext")

add_library(_usfs_common_methods OBJECT EXCLUDE_FROM_ALL
    src/common/usfs_common.c src/common/proxy_control.c src/common/instrumentation/config_normal.c)
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

add_subdirectory(src/client)
usfs_set_output(_usfs_client "${USFS_BIN_DIR}" usfs)
configure_file(src/client/usfs.pc.in "${USFS_GENERATED_DIR}/usfs.pc" @ONLY)

function(usfs_add_program target output)
    add_executable(${target} EXCLUDE_FROM_ALL ${ARGN})
    usfs_common_target(${target})
    usfs_set_output(${target} "${USFS_BIN_DIR}" "${output}")
endfunction()

usfs_add_program(_usfs_ctl usfsctl src/common/usfs_status.c src/cli/usfsctl.c $<TARGET_OBJECTS:_usfs_common_methods>)
target_link_libraries(_usfs_ctl PRIVATE cfg odm)
usfs_add_program(_usfs_coverage_ctl usfsctl src/common/usfs_status.c src/cli/usfsctl.c $<TARGET_OBJECTS:_usfs_coverage_methods>)
target_link_libraries(_usfs_coverage_ctl PRIVATE cfg odm)
target_compile_options(_usfs_coverage_ctl PRIVATE ${USFS_GCOV_FLAGS})
target_link_options(_usfs_coverage_ctl PRIVATE ${USFS_GCOV_FLAGS})
usfs_set_output(_usfs_coverage_ctl "${USFS_COVERAGE_BIN_DIR}" usfsctl)

foreach(_target IN ITEMS _usfs_ctl _usfs_coverage_ctl)
    target_compile_definitions(${_target} PRIVATE
        USFS_PACKAGE_VERSION="${USFS_PACKAGE_VERSION}"
        USFS_BUILD_REVISION="${USFS_BUILD_REVISION}")
endforeach()
foreach(_example IN ITEMS mirror memfs)
    usfs_add_program(_usfs_${_example} usfs_${_example} "examples/${_example}.c")
    target_link_libraries(_usfs_${_example} PRIVATE _usfs_client pthreads)
endforeach()

set(USFS_TRACE_FORMAT "${CMAKE_SOURCE_DIR}/packaging/aix/trace/usfs.trcfmt")
if(CMAKE_CROSSCOMPILING)
    set(USFS_TRACE_FORMAT_SHA256
        2756cd2e8786c517aa00ce2eeff48da8f8cdcb582b3f566af86de4eef38ef06f)
    add_custom_target(_usfs_trace_format
        COMMAND "${CMAKE_COMMAND}"
            -DUSFS_FILE=${USFS_TRACE_FORMAT}
            -DUSFS_EXPECTED_SHA256=${USFS_TRACE_FORMAT_SHA256}
            "-DUSFS_FILE_DESCRIPTION=trace-format template"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyPinnedFile.cmake"
        VERBATIM)
else()
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/usfs-trace-format.stamp"
        COMMAND trcrpt -c -t "${USFS_TRACE_FORMAT}"
        COMMAND "${CMAKE_COMMAND}" -E touch
            "${CMAKE_BINARY_DIR}/usfs-trace-format.stamp"
        DEPENDS "${USFS_TRACE_FORMAT}"
        VERBATIM)
    add_custom_target(_usfs_trace_format
        DEPENDS "${CMAKE_BINARY_DIR}/usfs-trace-format.stamp")
endif()

add_custom_target(_usfs_release_artifacts DEPENDS
    _usfs_kernel_release _usfs_cfgusfs _usfs_ucfgusfs _usfs_chusfs
    _usfs_client _usfs_ctl _usfs_trace_format
    _usfs_mirror _usfs_memfs)

# Testing build: the production core is reused, while instrumentation policy,
# methods, and private test tools are compiled as a separate variant.
usfs_kernel_objects(_usfs_kernel_testing_core ${USFS_KERNEL_CORE_SOURCES})
usfs_kernel_objects(_usfs_kernel_testing_instrumentation
    src/kernel/instrumentation/services.c
    src/kernel/instrumentation/adapter.c
    src/kernel/instrumentation/testing/runtime.c
    src/kernel/instrumentation/testing/fid_probe.c
    tests/kernel/aix/selftest.c)
target_compile_definitions(_usfs_kernel_testing_instrumentation PRIVATE USFS_TESTING)
target_include_directories(_usfs_kernel_testing_instrumentation PRIVATE
    "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/testing")

add_custom_command(
    OUTPUT "${USFS_TEST_BIN_DIR}/usfs.kext"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USFS_TEST_BIN_DIR}"
    COMMAND "${CMAKE_LINKER}" -b64 -e usfs_kext_entry
        -o "${USFS_TEST_BIN_DIR}/usfs.kext" "-bI:${USFS_KERNEX_IMPORT_FILE}" "-bI:${CMAKE_SOURCE_DIR}/src/kernel/proxy/proxy.exp"
        $<TARGET_OBJECTS:_usfs_kernel_testing_core>
        $<TARGET_OBJECTS:_usfs_kernel_testing_instrumentation>
        $<TARGET_OBJECTS:_usfs_kernel_hooks> -lsys -lcsys
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/verify-kernel.sh"
        testing "${USFS_TEST_BIN_DIR}/usfs.kext"
    DEPENDS _usfs_kernel_testing_core _usfs_kernel_testing_instrumentation
        $<TARGET_OBJECTS:_usfs_kernel_testing_core>
        $<TARGET_OBJECTS:_usfs_kernel_testing_instrumentation>
        $<TARGET_OBJECTS:_usfs_kernel_hooks>
        _usfs_kernel_hooks "${CMAKE_SOURCE_DIR}/scripts/aix/verify-kernel.sh"
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(_usfs_kernel_testing DEPENDS "${USFS_TEST_BIN_DIR}/usfs.kext")

# Test and coverage methods share fault instrumentation; only coverage emits gcov.
foreach(_variant IN ITEMS testing coverage)
    add_library(_usfs_${_variant}_methods OBJECT EXCLUDE_FROM_ALL
        src/common/usfs_common.c src/common/proxy_control.c src/common/instrumentation/config_testing.c)
    usfs_common_target(_usfs_${_variant}_methods)
    if(_variant STREQUAL coverage)
        target_compile_options(_usfs_${_variant}_methods PRIVATE ${USFS_GCOV_FLAGS})
    endif()

    foreach(_method IN ITEMS cfgusfs ucfgusfs chusfs)
        if(_method STREQUAL cfgusfs)
            set(_source src/cli/configure_device.c)
        elseif(_method STREQUAL ucfgusfs)
            set(_source src/cli/unconfigure_device.c)
        else()
            set(_source src/cli/change_device.c)
        endif()
        add_executable(_usfs_${_variant}_${_method} EXCLUDE_FROM_ALL
            "${_source}" $<TARGET_OBJECTS:_usfs_${_variant}_methods>)
        usfs_common_target(_usfs_${_variant}_${_method})
        target_link_libraries(_usfs_${_variant}_${_method} PRIVATE cfg odm)
        if(_variant STREQUAL coverage)
            target_compile_options(_usfs_${_variant}_${_method} PRIVATE ${USFS_GCOV_FLAGS})
            target_link_options(_usfs_${_variant}_${_method} PRIVATE ${USFS_GCOV_FLAGS})
        endif()
        usfs_set_output(_usfs_${_variant}_${_method}
            "${CMAKE_BINARY_DIR}/${_variant}/bin" "${_method}")
    endforeach()
endforeach()

function(usfs_add_test_tool name)
    add_executable(_usfs_testing_${name} EXCLUDE_FROM_ALL ${ARGN})
    usfs_common_target(_usfs_testing_${name})
    target_include_directories(_usfs_testing_${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/harness")
    usfs_set_output(_usfs_testing_${name} "${USFS_TEST_BIN_DIR}" "${name}")
endfunction()
usfs_add_test_tool(usfs_probe tests/tools/usfs_probe.c)
usfs_add_test_tool(usfs_testctl tests/tools/usfs_testctl.c)
usfs_add_test_tool(usfs_scenario_daemon tests/tools/usfs_scenario_daemon.c src/common/usfs_validate.c)
usfs_add_test_tool(usfs_usercopy_test tests/harness/tap.c
    src/common/usfs_status.c tests/tools/usfs_usercopy_test.c)
usfs_add_test_tool(usfs_io_probe tests/tools/usfs_io_probe.c)
usfs_add_test_tool(usfs_fid_probe tests/tools/usfs_fid_probe.c)
usfs_add_test_tool(usfs_access_probe tests/tools/usfs_access_probe.c)
usfs_add_test_tool(usfs_overhead tests/performance/benchmark.c)
target_link_libraries(_usfs_testing_usfs_overhead PRIVATE _usfs_client pthreads)
usfs_add_test_tool(usfs_mt_test_daemon tests/tools/usfs_mt_test_daemon.c
    tests/helpers/namespace_client.c)
target_link_libraries(_usfs_testing_usfs_mt_test_daemon PRIVATE _usfs_client pthreads)
target_include_directories(_usfs_testing_usfs_mt_test_daemon PRIVATE
    "${CMAKE_SOURCE_DIR}/src/client/include/usfs")
usfs_add_test_tool(usfs_path_test_daemon tests/tools/usfs_path_test_daemon.c tests/harness/tap.c)
target_link_libraries(_usfs_testing_usfs_path_test_daemon PRIVATE _usfs_client pthreads)

add_custom_target(_usfs_testing_artifacts DEPENDS
    _usfs_kernel_testing
    _usfs_testing_cfgusfs _usfs_testing_ucfgusfs _usfs_testing_chusfs
    _usfs_testing_usfs_testctl _usfs_testing_usfs_scenario_daemon
    _usfs_testing_usfs_usercopy_test _usfs_testing_usfs_io_probe _usfs_testing_usfs_fid_probe
    _usfs_testing_usfs_access_probe _usfs_testing_usfs_mt_test_daemon _usfs_testing_usfs_path_test_daemon
    _usfs_testing_usfs_overhead
    _usfs_mirror _usfs_memfs _usfs_testing_usfs_probe _usfs_ctl)

# AIX userspace unit programs.
set(USFS_UNIT_TARGETS "")
set(USFS_COVERAGE_UNIT_TARGETS "")
function(usfs_add_unit name kind)
    set(_sources ${ARGN})
    foreach(_variant IN ITEMS normal coverage)
        if(_variant STREQUAL normal)
            set(_target _usfs_unit_${name})
            set(_directory "${CMAKE_BINARY_DIR}/tests")
        else()
            set(_target _usfs_coverage_unit_${name})
            set(_directory "${CMAKE_BINARY_DIR}/coverage/aix-unit")
        endif()
        add_executable(${_target} EXCLUDE_FROM_ALL ${_sources})
        target_compile_options(${_target} PRIVATE -maix64 -Wall -Wextra -Werror)
        target_include_directories(${_target} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/common" "${CMAKE_SOURCE_DIR}/src/kernel"
            "${CMAKE_SOURCE_DIR}/tests/harness"
            "${CMAKE_SOURCE_DIR}/tests/unit/aix")
        if(kind STREQUAL "KERNEL")
            target_compile_options(${_target} PRIVATE -O2 -Wno-unused-function)
            target_compile_definitions(${_target} PRIVATE
                _KERNEL _ALL_SOURCE _POSIX_SOURCE USFS_TRACE_DISABLED)
        elseif(kind STREQUAL "CLIENT")
            target_compile_options(${_target} PRIVATE -pthread)
            target_include_directories(${_target} PRIVATE
                "${CMAKE_SOURCE_DIR}/src/client/include"
                "${CMAKE_SOURCE_DIR}/src/client/include/usfs")
            target_compile_definitions(${_target} PRIVATE _POSIX_C_SOURCE=200809L)
            target_link_libraries(${_target} PRIVATE pthreads)
        endif()
        if(_variant STREQUAL coverage)
            target_compile_options(${_target} PRIVATE ${USFS_GCOV_FLAGS})
            target_link_options(${_target} PRIVATE ${USFS_GCOV_FLAGS})
            list(APPEND USFS_COVERAGE_UNIT_TARGETS ${_target})
        else()
            list(APPEND USFS_UNIT_TARGETS ${_target})
        endif()
        usfs_set_output(${_target} "${_directory}" "${name}")
    endforeach()
    set(USFS_UNIT_TARGETS "${USFS_UNIT_TARGETS}" PARENT_SCOPE)
    set(USFS_COVERAGE_UNIT_TARGETS "${USFS_COVERAGE_UNIT_TARGETS}" PARENT_SCOPE)
endfunction()

usfs_add_unit(tap_selftest PORTABLE tests/harness/tap.c tests/harness/tap_selftest.c)
usfs_add_unit(configuration_methods_test PORTABLE tests/harness/tap.c
    tests/unit/aix/configuration_methods_test.c)
usfs_add_unit(device_node_policy_test PORTABLE tests/harness/tap.c
    src/common/instrumentation/config_normal.c src/common/proxy_control.c tests/unit/aix/device_node_policy_test.c)
target_compile_options(_usfs_unit_device_node_policy_test PRIVATE -pthread)
target_compile_options(_usfs_coverage_unit_device_node_policy_test PRIVATE -pthread)
target_link_libraries(_usfs_unit_device_node_policy_test PRIVATE cfg odm pthreads)
target_link_libraries(_usfs_coverage_unit_device_node_policy_test PRIVATE cfg odm pthreads)
usfs_add_unit(coverage_container_file_test PORTABLE tests/harness/tap.c
    tests/unit/aix/coverage_container_file_test.c src/common/usfs_common.c
    src/common/proxy_control.c src/common/instrumentation/config_normal.c)
target_link_libraries(_usfs_unit_coverage_container_file_test PRIVATE cfg odm)
target_link_libraries(_usfs_coverage_unit_coverage_container_file_test PRIVATE cfg odm)
usfs_add_unit(io_probe_security_test PORTABLE tests/harness/tap.c
    tests/unit/aix/io_probe_security_test.c)
usfs_add_unit(protocol_test PORTABLE tests/harness/tap.c src/common/usfs_validate.c
    src/common/usfs_status.c tests/unit/protocol_test.c)
usfs_add_unit(corpus_test PORTABLE tests/harness/tap.c src/common/usfs_validate.c
    tests/unit/corpus_test.c)
usfs_add_unit(usfs_client_test CLIENT tests/harness/tap.c src/common/usfs_validate.c
    tests/unit/aix/usfs_client_test.c)
usfs_add_unit(mirror_readdir_test CLIENT tests/harness/tap.c
    tests/unit/aix/mirror_readdir_test.c)
target_link_libraries(_usfs_unit_mirror_readdir_test PRIVATE _usfs_client)
target_link_libraries(_usfs_coverage_unit_mirror_readdir_test PRIVATE _usfs_client_coverage)
usfs_add_unit(memfs_fid_test CLIENT tests/harness/tap.c tests/unit/aix/memfs_fid_test.c)
target_link_libraries(_usfs_unit_memfs_fid_test PRIVATE _usfs_client)
target_link_libraries(_usfs_coverage_unit_memfs_fid_test PRIVATE _usfs_client_coverage)
usfs_add_unit(synchronized_test PORTABLE tests/harness/tap.c tests/unit/synchronized_test.c)
usfs_add_unit(soak_worker_test PORTABLE tests/harness/tap.c tests/unit/soak_worker_test.c)
usfs_add_unit(fake_kernel_test KERNEL tests/harness/tap.c tests/unit/aix/support/fake_kernel.c
    tests/unit/aix/fake_kernel_test.c)
usfs_add_unit(access_attr_test KERNEL tests/harness/tap.c tests/unit/aix/support/fake_kernel.c
    src/common/usfs_validate.c src/kernel/fs/helpers.c tests/unit/aix/access_attr_test.c)
usfs_add_unit(kernel_common_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/fs/helpers.c tests/unit/aix/support/fake_kernel.c tests/unit/aix/kernel_common_test.c)
usfs_add_unit(open_state_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/fs/helpers.c tests/unit/aix/support/fake_kernel.c tests/unit/aix/open_state_test.c)
usfs_add_unit(readdir_identity_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    tests/unit/aix/readdir_identity_test.c)
usfs_add_unit(request_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/device/protocol/request_build.c tests/unit/aix/support/fake_kernel.c tests/unit/aix/request_test.c)
usfs_add_unit(request_state_test KERNEL tests/harness/tap.c src/kernel/device/protocol/request_state.c
    tests/unit/aix/request_state_test.c)
usfs_add_unit(request_wait_test KERNEL tests/harness/tap.c src/kernel/device/protocol/request_wait.c
    src/kernel/device/protocol/request_state.c tests/unit/aix/request_wait_test.c)
usfs_add_unit(io_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/fs/io_helpers.c tests/unit/aix/io_test.c)
usfs_add_unit(cache_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/fs/io_helpers.c tests/unit/aix/cache_test.c)
usfs_add_unit(pager_completion_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/fs/io_helpers.c tests/unit/aix/pager_completion_test.c)
usfs_add_unit(mapping_pager_test KERNEL tests/harness/tap.c src/kernel/fs/io_helpers.c
    tests/unit/aix/mapping_pager_test.c)
usfs_add_unit(unsupported_ops_test KERNEL tests/harness/tap.c tests/unit/aix/unsupported_ops_test.c)
usfs_add_unit(lockctl_test KERNEL tests/harness/tap.c tests/unit/aix/lockctl_test.c)
# Keep the trivial stubs callable for direct unit assertions.
target_compile_options(_usfs_unit_unsupported_ops_test PRIVATE -O0)
target_compile_options(_usfs_coverage_unit_unsupported_ops_test PRIVATE -O0)
usfs_add_unit(namespace_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    src/kernel/fs/helpers.c tests/unit/aix/namespace_test.c)
usfs_add_unit(device_transport_test KERNEL tests/harness/tap.c
    src/kernel/device/protocol/transport_validation.c tests/unit/aix/device_transport_test.c)
usfs_add_unit(device_operations_test KERNEL tests/harness/tap.c
    src/common/usfs_validate.c src/kernel/device/protocol/transport_validation.c tests/unit/aix/device_operations_test.c)
# Preserve direct header-operation calls for negative-path assertions.
target_compile_options(_usfs_unit_device_operations_test PRIVATE -O0)
target_compile_options(_usfs_coverage_unit_device_operations_test PRIVATE -O0)
usfs_add_unit(runtime_config_test KERNEL tests/harness/tap.c
    src/kernel/device/runtime_config.c tests/unit/aix/runtime_config_test.c)
usfs_add_unit(observability_test KERNEL tests/harness/tap.c src/common/usfs_status.c
    tests/unit/aix/observability_test.c)
usfs_add_unit(mount_lifecycle_test KERNEL tests/harness/tap.c
    src/kernel/instrumentation/adapter.c tests/unit/aix/mount_lifecycle_test.c)
usfs_add_unit(mount_lifecycle_deferred_test KERNEL tests/harness/tap.c
    src/kernel/instrumentation/adapter.c tests/unit/aix/mount_lifecycle_test.c)
target_compile_definitions(_usfs_unit_mount_lifecycle_deferred_test PRIVATE USFS_COVERAGE)
target_compile_definitions(_usfs_coverage_unit_mount_lifecycle_deferred_test PRIVATE USFS_COVERAGE)
usfs_add_unit(vfs_root_test KERNEL tests/harness/tap.c tests/unit/aix/vfs_root_test.c)
usfs_add_unit(fid_format_test KERNEL tests/harness/tap.c tests/unit/aix/fid_format_test.c)
usfs_add_unit(fid_ops_test KERNEL tests/harness/tap.c src/common/usfs_validate.c
    tests/unit/aix/fid_ops_test.c)
usfs_add_unit(forced_unmount_test KERNEL tests/harness/tap.c tests/unit/aix/forced_unmount_test.c)
usfs_add_unit(proxy_test KERNEL tests/harness/tap.c tests/unit/aix/proxy_test.c)
usfs_add_unit(gcov_runtime_test KERNEL tests/harness/tap.c tests/unit/aix/gcov_runtime_test.c)
usfs_add_unit(lifecycle_state_test KERNEL tests/harness/tap.c src/kernel/lifecycle/state.c
    tests/unit/aix/lifecycle_state_test.c)

add_custom_target(_usfs_unit_programs DEPENDS ${USFS_UNIT_TARGETS})
add_custom_target(_usfs_coverage_unit_programs DEPENDS ${USFS_COVERAGE_UNIT_TARGETS})

# Coverage builds support arbitrary source and binary directories on AIX.
usfs_kernel_objects(_usfs_kernel_coverage_core ${USFS_KERNEL_CORE_SOURCES})
target_compile_options(_usfs_kernel_coverage_core PRIVATE ${USFS_GCOV_FLAGS})
usfs_kernel_objects(_usfs_kernel_coverage_instrumentation
    src/kernel/instrumentation/services.c src/kernel/instrumentation/adapter.c
    src/kernel/instrumentation/hooks.c
    src/kernel/instrumentation/testing/runtime.c
    src/kernel/instrumentation/testing/fid_probe.c tests/kernel/aix/selftest.c)
target_compile_options(_usfs_kernel_coverage_instrumentation PRIVATE ${USFS_GCOV_FLAGS})
target_compile_definitions(_usfs_kernel_coverage_instrumentation PRIVATE USFS_COVERAGE USFS_TESTING)
target_include_directories(_usfs_kernel_coverage_instrumentation PRIVATE
    "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/coverage"
    "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/testing")
add_library(_usfs_kernel_coverage_runtime OBJECT EXCLUDE_FROM_ALL
    src/kernel/instrumentation/coverage/runtime.c
    src/kernel/instrumentation/coverage/control.c)
target_compile_options(_usfs_kernel_coverage_runtime PRIVATE ${USFS_DIAGNOSTIC_FLAGS})
target_compile_definitions(_usfs_kernel_coverage_runtime PRIVATE
    _KERNEL _ALL_SOURCE _POSIX_SOURCE USFS_COVERAGE)
target_include_directories(_usfs_kernel_coverage_runtime PRIVATE
    "${USFS_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/src/kernel"
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/coverage")
add_dependencies(_usfs_kernel_coverage_runtime _usfs_errids)

set(USFS_COVERAGE_REGISTRY "${CMAKE_BINARY_DIR}/coverage/generated/registry.c")
add_custom_command(
    OUTPUT "${USFS_COVERAGE_REGISTRY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/coverage/generated"
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/generate-gcov-registry.sh"
        "${USFS_COVERAGE_REGISTRY}"
        $<TARGET_OBJECTS:_usfs_kernel_coverage_core>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_instrumentation>
    DEPENDS _usfs_kernel_coverage_core _usfs_kernel_coverage_instrumentation
        "${CMAKE_SOURCE_DIR}/scripts/aix/generate-gcov-registry.sh"
        $<TARGET_OBJECTS:_usfs_kernel_coverage_core>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_instrumentation>
    COMMAND_EXPAND_LISTS VERBATIM)
add_library(_usfs_kernel_coverage_registry OBJECT EXCLUDE_FROM_ALL "${USFS_COVERAGE_REGISTRY}")
target_compile_options(_usfs_kernel_coverage_registry PRIVATE ${USFS_DIAGNOSTIC_FLAGS})
target_compile_definitions(_usfs_kernel_coverage_registry PRIVATE
    _KERNEL _ALL_SOURCE _POSIX_SOURCE USFS_COVERAGE)
target_include_directories(_usfs_kernel_coverage_registry PRIVATE
    "${CMAKE_SOURCE_DIR}/src/kernel" "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/coverage")

add_custom_command(
    OUTPUT "${USFS_COVERAGE_BIN_DIR}/usfs.kext"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USFS_COVERAGE_BIN_DIR}"
    COMMAND "${CMAKE_LINKER}" -b64 -e usfs_kext_entry
        -o "${USFS_COVERAGE_BIN_DIR}/usfs.kext" "-bI:${USFS_KERNEX_IMPORT_FILE}" "-bI:${CMAKE_SOURCE_DIR}/src/kernel/proxy/proxy.exp"
        $<TARGET_OBJECTS:_usfs_kernel_coverage_core>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_instrumentation>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_runtime>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_registry>
        -lsys -lcsys
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/verify-kernel.sh"
        coverage "${USFS_COVERAGE_BIN_DIR}/usfs.kext"
    DEPENDS _usfs_kernel_coverage_core _usfs_kernel_coverage_instrumentation
        _usfs_kernel_coverage_runtime _usfs_kernel_coverage_registry
        $<TARGET_OBJECTS:_usfs_kernel_coverage_core>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_instrumentation>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_runtime>
        $<TARGET_OBJECTS:_usfs_kernel_coverage_registry>
        "${CMAKE_SOURCE_DIR}/scripts/aix/verify-kernel.sh"
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(_usfs_kernel_coverage DEPENDS "${USFS_COVERAGE_BIN_DIR}/usfs.kext")

add_library(_usfs_client_coverage STATIC EXCLUDE_FROM_ALL
    src/client/lib/usfs_client.c src/common/usfs_validate.c)
usfs_common_target(_usfs_client_coverage)
target_include_directories(_usfs_client_coverage PUBLIC "${CMAKE_SOURCE_DIR}/src/client/include")
target_include_directories(_usfs_client_coverage PRIVATE "${CMAKE_SOURCE_DIR}/src/client/include/usfs")
set_target_properties(_usfs_client_coverage PROPERTIES PREFIX "lib" SUFFIX ".a")
target_compile_options(_usfs_client_coverage PRIVATE ${USFS_GCOV_FLAGS})
target_link_options(_usfs_client_coverage INTERFACE ${USFS_GCOV_FLAGS})
target_link_libraries(_usfs_client_coverage PUBLIC pthreads)
usfs_set_output(_usfs_client_coverage "${USFS_COVERAGE_BIN_DIR}" usfs)
foreach(_example IN ITEMS mirror memfs)
    add_executable(_usfs_coverage_${_example} EXCLUDE_FROM_ALL "examples/${_example}.c")
    usfs_common_target(_usfs_coverage_${_example})
    target_compile_options(_usfs_coverage_${_example} PRIVATE ${USFS_GCOV_FLAGS})
    target_link_options(_usfs_coverage_${_example} PRIVATE ${USFS_GCOV_FLAGS})
    target_link_libraries(_usfs_coverage_${_example} PRIVATE _usfs_client_coverage pthreads)
    usfs_set_output(_usfs_coverage_${_example} "${USFS_COVERAGE_BIN_DIR}" "usfs_${_example}")
endforeach()
usfs_add_test_tool(usfs_proxy_testctl tests/tools/usfs_proxy_testctl.c)
target_sources(_usfs_testing_usfs_proxy_testctl PRIVATE $<TARGET_OBJECTS:_usfs_common_methods>)
target_link_libraries(_usfs_testing_usfs_proxy_testctl PRIVATE cfg odm)
add_dependencies(_usfs_testing_artifacts _usfs_testing_usfs_proxy_testctl)
usfs_add_test_tool(usfs_coverage tests/tools/usfs_coverage.c)
target_sources(_usfs_testing_usfs_coverage PRIVATE $<TARGET_OBJECTS:_usfs_common_methods>)
target_link_libraries(_usfs_testing_usfs_coverage PRIVATE cfg odm)
usfs_set_output(_usfs_testing_usfs_coverage "${USFS_COVERAGE_BIN_DIR}" usfs_coverage)

add_custom_target(_usfs_coverage_artifacts DEPENDS
    _usfs_kernel_coverage _usfs_coverage_unit_programs
    _usfs_client_coverage _usfs_coverage_mirror
    _usfs_coverage_memfs _usfs_testing_usfs_coverage
    _usfs_coverage_cfgusfs _usfs_coverage_ucfgusfs _usfs_coverage_chusfs
    _usfs_testing_usfs_testctl _usfs_testing_usfs_scenario_daemon
    _usfs_testing_usfs_usercopy_test _usfs_testing_usfs_io_probe _usfs_testing_usfs_fid_probe
    _usfs_testing_usfs_access_probe _usfs_testing_usfs_mt_test_daemon _usfs_testing_usfs_path_test_daemon
    _usfs_testing_usfs_probe _usfs_coverage_ctl)

set(USFS_RPM_FILE
    "usfs-${USFS_PACKAGE_VERSION}-${USFS_PACKAGE_RELEASE}.aix${CMAKE_HOST_SYSTEM_VERSION}.ppc.rpm")
set(USFS_RPM_PATH "${USFS_PACKAGES_DIR}/${USFS_RPM_FILE}")
add_custom_target(package ALL
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/build-package.sh"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
        "${USFS_PACKAGE_VERSION}" "${USFS_PACKAGE_RELEASE}"
        "${USFS_BUILD_REVISION}" "${USFS_BUILD_DIRTY}"
        "${USFS_PACKAGE_PRE_FAULT}" "${USFS_PACKAGE_EXTRA_REQUIRES}"
        "${CMAKE_HOST_SYSTEM_VERSION}"
    BYPRODUCTS "${USFS_RPM_PATH}" "${USFS_RPM_PATH}.sha256"
    DEPENDS _usfs_release_artifacts
        "${CMAKE_SOURCE_DIR}/packaging/aix/rpm/usfs.spec"
        "${CMAKE_SOURCE_DIR}/scripts/aix/build-package.sh"
    VERBATIM)

add_custom_target(test
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/run-target.sh"
        test "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
    DEPENDS _usfs_release_artifacts _usfs_testing_artifacts _usfs_unit_programs
    USES_TERMINAL VERBATIM)

add_custom_target(test-e2e
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/run-target.sh"
        test-e2e "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
    DEPENDS _usfs_release_artifacts _usfs_testing_artifacts
    USES_TERMINAL VERBATIM)

add_custom_target(coverage
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/run-target.sh"
        coverage "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
        "${USFS_COVERAGE_RUN}" "${USFS_BUILD_REVISION}" "${USFS_BUILD_DIRTY}"
    DEPENDS _usfs_release_artifacts _usfs_testing_artifacts _usfs_coverage_artifacts
    USES_TERMINAL VERBATIM)

set(USFS_SOAK_NAME
    "usfs-soak-${USFS_PACKAGE_VERSION}-${USFS_SOAK_REVISION}-aix${CMAKE_HOST_SYSTEM_VERSION}-ppc64")
set(USFS_SOAK_PATH "${USFS_PACKAGES_DIR}/${USFS_SOAK_NAME}.tar.gz")
foreach(_tool IN ITEMS usfs_scenario_daemon usfs_io_probe usfs_soak_worker usfs_admin_probe)
    string(REPLACE "usfs_" "" _short "${_tool}")
    add_executable(_usfs_soak_${_short} EXCLUDE_FROM_ALL "tests/tools/${_tool}.c")
    usfs_common_target(_usfs_soak_${_short})
    usfs_set_output(_usfs_soak_${_short} "${CMAKE_BINARY_DIR}/soak-bin" "${_tool}")
endforeach()
add_custom_target(_usfs_soak_tools DEPENDS
    # The scenario peer uses the shared validator for the current wire contract.
    _usfs_soak_scenario_daemon _usfs_soak_io_probe
    _usfs_soak_soak_worker _usfs_soak_admin_probe)
target_sources(_usfs_soak_scenario_daemon PRIVATE src/common/usfs_validate.c)
add_custom_target(soak
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/build-soak.sh"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
        "${USFS_PACKAGE_VERSION}" "${USFS_PACKAGE_RELEASE}"
        "${USFS_SOAK_REVISION}" "${USFS_RPM_FILE}"
        "${CMAKE_HOST_SYSTEM_VERSION}"
    BYPRODUCTS "${USFS_SOAK_PATH}" "${USFS_SOAK_PATH}.sha256"
    DEPENDS package _usfs_soak_tools
        "${CMAKE_SOURCE_DIR}/scripts/aix/build-soak.sh"
    VERBATIM)

message(STATUS "USFS public targets: package (default), test, test-e2e, coverage, soak")
message(STATUS "Artifacts will be written below ${CMAKE_BINARY_DIR}")

# Fingerprint resident code independently of the implementation's release revision.
set(_proxy_identity_input "")
file(GLOB_RECURSE _implementation_identity_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/kernel/*" "${CMAKE_SOURCE_DIR}/src/common/*")
list(SORT _implementation_identity_sources)
set(_implementation_identity_input "")
file(SHA256 "${CMAKE_SOURCE_DIR}/CMakeLists.txt" _build_recipe_hash)
string(APPEND _implementation_identity_input "${_build_recipe_hash}")
foreach(_source IN LISTS _implementation_identity_sources)
    if(NOT IS_DIRECTORY "${_source}")
        file(SHA256 "${_source}" _source_hash)
        string(APPEND _implementation_identity_input "${_source_hash}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
    endif()
endforeach()
foreach(_variant IN ITEMS release testing coverage)
    string(SHA256 _implementation_identity "${_implementation_identity_input}:${_variant}")
    string(SUBSTRING "${_implementation_identity}" 0 40 _implementation_identity)
    if(_variant STREQUAL release)
        set(_implementation_core_target _usfs_kernel_release_objects)
    else()
        set(_implementation_core_target _usfs_kernel_${_variant}_core)
    endif()
    target_compile_definitions(${_implementation_core_target} PRIVATE
        USFS_IMPLEMENTATION_BUILD_ID="${_implementation_identity}")
endforeach()

string(APPEND _proxy_identity_input "${_build_recipe_hash}")
foreach(_source IN ITEMS src/kernel/proxy/proxy.c src/common/usfs_proxy.h src/kernel/instrumentation/proxy.h)
    file(SHA256 "${CMAKE_SOURCE_DIR}/${_source}" _source_hash)
    string(APPEND _proxy_identity_input "${_source_hash}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${_source}")
endforeach()
# The implementation imports only these permanently resident kernel services.
foreach(_variant IN ITEMS release testing coverage)
    if(_variant STREQUAL release)
        set(_proxy_dir "${USFS_BIN_DIR}")
    elseif(_variant STREQUAL testing)
        set(_proxy_dir "${USFS_TEST_BIN_DIR}")
    else()
        set(_proxy_dir "${USFS_COVERAGE_BIN_DIR}")
    endif()
    usfs_kernel_objects(_usfs_proxy_${_variant}_objects src/kernel/proxy/proxy.c)
    string(SHA256 _proxy_identity "${_proxy_identity_input}:${_variant}")
    string(SUBSTRING "${_proxy_identity}" 0 40 _proxy_identity)
    target_compile_definitions(_usfs_proxy_${_variant}_objects PRIVATE
        USFS_PROXY_VARIANT="${_variant}" USFS_PROXY_BUILD_ID="${_proxy_identity}")
    if(NOT _variant STREQUAL release)
        target_compile_definitions(_usfs_proxy_${_variant}_objects PRIVATE USFS_TESTING)
    endif()
    set(_proxy_extras)
    set(_proxy_dependencies)
    if(_variant STREQUAL coverage)
        target_compile_definitions(_usfs_proxy_coverage_objects PRIVATE USFS_COVERAGE)
        target_compile_options(_usfs_proxy_coverage_objects PRIVATE ${USFS_GCOV_FLAGS})
        usfs_kernel_objects(_usfs_proxy_coverage_runtime src/kernel/instrumentation/coverage/runtime.c)
        target_include_directories(_usfs_proxy_coverage_runtime PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/coverage")
        set(_proxy_registry "${CMAKE_BINARY_DIR}/coverage/generated/proxy-registry.c")
        add_custom_command(OUTPUT "${_proxy_registry}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/coverage/generated"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/aix/generate-gcov-registry.sh"
                "${_proxy_registry}" $<TARGET_OBJECTS:_usfs_proxy_coverage_objects>
            DEPENDS _usfs_proxy_coverage_objects $<TARGET_OBJECTS:_usfs_proxy_coverage_objects>
            COMMAND_EXPAND_LISTS VERBATIM)
        usfs_kernel_objects(_usfs_proxy_coverage_registry "${_proxy_registry}")
        target_include_directories(_usfs_proxy_coverage_registry PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel/instrumentation/coverage")
        list(APPEND _proxy_extras $<TARGET_OBJECTS:_usfs_proxy_coverage_runtime> $<TARGET_OBJECTS:_usfs_proxy_coverage_registry>)
        list(APPEND _proxy_dependencies _usfs_proxy_coverage_runtime _usfs_proxy_coverage_registry)
    endif()
    add_custom_command(OUTPUT "${_proxy_dir}/usfs_proxy.kext"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_proxy_dir}"
        COMMAND "${CMAKE_LINKER}" -b64 -e usfs_proxy_entry
            -o "${_proxy_dir}/usfs_proxy.kext"
            "-bI:${USFS_KERNEX_IMPORT_FILE}"
            "-bE:${CMAKE_SOURCE_DIR}/src/kernel/proxy/proxy.exp"
            $<TARGET_OBJECTS:_usfs_proxy_${_variant}_objects> ${_proxy_extras} -lsys -lcsys
        DEPENDS _usfs_proxy_${_variant}_objects ${_proxy_dependencies} ${_proxy_extras}
            $<TARGET_OBJECTS:_usfs_proxy_${_variant}_objects>
            "${CMAKE_SOURCE_DIR}/src/kernel/proxy/proxy.exp"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(_usfs_proxy_${_variant} DEPENDS "${_proxy_dir}/usfs_proxy.kext")
    add_dependencies(_usfs_kernel_${_variant} _usfs_proxy_${_variant})
endforeach()
