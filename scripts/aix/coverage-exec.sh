#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Launch one instrumented AIX userspace daemon with a PID-specific GCOV
#   output directory so concurrent and repeated E2E processes cannot overwrite
#   one another's profiles.
#
# Usage:
#   Set the daemon and active coverage run, then pass any daemon arguments:
#     USFS_COVERAGE_DAEMON=/usr/sbin/usfs_memfs \
#       USFS_COVERAGE_SUITE=nightly \
#       bash scripts/aix/coverage-exec.sh
#
#   This wrapper is normally selected automatically by the E2E scripts.

set -eu

if [ -z "${USFS_COVERAGE_DAEMON:-}" ] ||
   [ -z "${USFS_COVERAGE_SUITE:-}" ]; then
    echo "coverage-exec requires USFS_COVERAGE_DAEMON and USFS_COVERAGE_SUITE" >&2
    exit 2
fi

: "${USFS_BINARY_DIR:?USFS_BINARY_DIR is required}"
profile_dir="$USFS_BINARY_DIR/coverage/user-data/${USFS_COVERAGE_SUITE}/$$"
mkdir -p "$profile_dir"
export GCOV_PREFIX="$profile_dir"
# AIX libgcov does not create the nested source/build directories retained by
# a zero strip count. Flatten each process's uniquely named gcda files into
# its isolated profile directory instead.
export GCOV_PREFIX_STRIP=999
exec "$USFS_COVERAGE_DAEMON" "$@"
