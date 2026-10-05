# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

set(CMAKE_SYSTEM_NAME AIX)
set(CMAKE_SYSTEM_VERSION 7.2)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)

set(AIX_CROSS_PREFIX /usr/local/aix CACHE PATH "Installation prefix of the AIX cross-compiler")
set(AIX_CROSS_TARGET powerpc-ibm-aix7.2 CACHE STRING "AIX cross-compiler target triplet")

set(CMAKE_SYSROOT "${AIX_CROSS_PREFIX}/sysroot")

set(CMAKE_C_COMPILER "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-gcc")
set(CMAKE_AR "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-ar")
set(CMAKE_LINKER "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-ld")
set(CMAKE_NM "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-nm")
set(CMAKE_OBJCOPY "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-objcopy")
set(CMAKE_OBJDUMP "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-objdump")
set(CMAKE_RANLIB "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-ranlib")
set(CMAKE_STRIP "${AIX_CROSS_PREFIX}/bin/${AIX_CROSS_TARGET}-strip")

# The collected sysroot intentionally supports 64-bit AIX builds only.
set(CMAKE_C_FLAGS_INIT "-maix64")

# CMake's AIX-GNU module assumes the native AIX linker and injects options
# which GNU ld does not accept.  Load the cross-linker compatibility rules
# after CMake has initialized its platform defaults.
set(CMAKE_USER_MAKE_RULES_OVERRIDE_C "${CMAKE_CURRENT_LIST_DIR}/aix72-gcc-kext-rules.cmake")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" "${AIX_CROSS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
