#!/usr/bin/env bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Validate Sonar project keys and install scanner signature tools.
# Usage: bash tests/ci/install-sonar-scan-tools.sh

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/aix-common.sh"

install_gpg_tools()
{
    # The scan action verifies its downloaded scanner with GPG.
    if ! command -v gpg >/dev/null 2>&1; then
        dnf install -y gnupg2
    fi

    if ! command -v dirmngr >/dev/null 2>&1; then
        dnf install -y gnupg2
    fi

    command -v gpg >/dev/null
    command -v dirmngr >/dev/null
}

require_environment_variable SONAR_ORGANIZATION
require_environment_variable SONAR_PROJECT_KEY
install_gpg_tools
