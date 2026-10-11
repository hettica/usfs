#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Emit a deterministic SHA-256 manifest for every tracked input that can
#   affect coverage instrumentation, test execution, collection, or reporting.
#
# Usage:
#   From the repository root on AIX:
#     bash scripts/aix/source-manifest.sh
#
# Output:
#   One "<sha256>  <relative-path>" record per file, sorted by path. The
#   Perl SHA helper provides the same digest format throughout the archive.

set -eu

find CMakeLists.txt cmake src examples tests scripts -type f -print |
    LC_ALL=C sort |
    while IFS= read -r path; do
        perl scripts/aix/sha256.pl "$path"
    done
