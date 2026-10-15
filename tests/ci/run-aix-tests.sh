#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Copy sources to AIX and run ordinary and end-to-end tests.
# Usage: bash tests/ci/run-aix-tests.sh

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/aix-common.sh"

load_aix_ssh_options
upload_sources_to_aix
configure_aix_build

run_aix_ssh_command "
    set -eu
    cd '$AIX_SOURCE_DIR/build'
    make test
    make test-e2e
"
