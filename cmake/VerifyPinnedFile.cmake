# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

if(NOT DEFINED USFS_FILE OR NOT DEFINED USFS_EXPECTED_SHA256 OR
   NOT DEFINED USFS_FILE_DESCRIPTION)
    message(FATAL_ERROR "VerifyPinnedFile.cmake requires a file, hash, and description")
endif()

if(NOT EXISTS "${USFS_FILE}")
    message(FATAL_ERROR "Pinned ${USFS_FILE_DESCRIPTION} is missing: ${USFS_FILE}")
endif()

file(SHA256 "${USFS_FILE}" _usfs_actual_sha256)
if(NOT _usfs_actual_sha256 STREQUAL USFS_EXPECTED_SHA256)
    message(FATAL_ERROR
        "Pinned ${USFS_FILE_DESCRIPTION} changed. Expected "
        "${USFS_EXPECTED_SHA256}, got ${_usfs_actual_sha256}. "
        "Regenerate usfs_errids.h with errupdate -h -n on AIX, verify the "
        "template/header pair, and update the approved checksums.")
endif()
