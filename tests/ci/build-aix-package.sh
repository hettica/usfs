#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Copy sources to AIX and build the production RPM.
# Usage: bash tests/ci/build-aix-package.sh

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/aix-common.sh"

load_aix_ssh_options
upload_sources_to_aix
configure_aix_build

run_aix_ssh_command "cd '$AIX_SOURCE_DIR/build' && make"
