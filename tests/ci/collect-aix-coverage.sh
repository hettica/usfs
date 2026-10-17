#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Run instrumented AIX workloads and download LCOV and Sonar reports.
# Usage: bash tests/ci/collect-aix-coverage.sh

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/aix-common.sh"

generate_aix_coverage_reports()
{
    run_aix_ssh_command "
        set -eu
        cd '$AIX_SOURCE_DIR'
        cmake -S . -B build -DUSFS_COVERAGE_RUN=ci
        cd build
        make coverage
        if ! grep -qx complete coverage/ci/report-status.txt; then
            echo 'Failed to publish coverage: collection is incomplete' >&2
            exit 1
        fi
        perl '../tests/ci/lcov-to-sonar.pl' \
            '$AIX_SOURCE_DIR' coverage/ci/all.info \
            > coverage/ci/sonar-coverage.xml
    "
}

download_aix_coverage_reports()
{
    mkdir -p "$RUNNER_SOURCE_DIR/out/coverage"
    run_aix_ssh_command "
        cd '$AIX_SOURCE_DIR/build/coverage/ci' &&
        tar -cf - sonar-coverage.xml all.info report-status.txt
    " | tar -C "$RUNNER_SOURCE_DIR/out/coverage" -xf -
}

load_aix_ssh_options
upload_sources_to_aix
generate_aix_coverage_reports
download_aix_coverage_reports
