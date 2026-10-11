#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Restore the selected ordinary release after a coverage campaign.
# Usage: bash scripts/aix/restore-release.sh SOURCE BUILD

set -eu
unset USFS_COVERAGE_SUITE USFS_COVERAGE_EVENT
source_directory=$1
build_directory=$2
bash "$source_directory/scripts/aix/cleanup.sh"
if lsdev -l usfs0 2>/dev/null | grep -q '^usfs0'; then
    rmdev -l usfs0
fi
bash "$source_directory/scripts/aix/install-variant.sh" release "$source_directory" "$build_directory"
bash "$source_directory/scripts/aix/setup.sh"
cmp "$build_directory/bin/usfs.kext" /usr/lib/drivers/usfs/usfs.kext
/usr/sbin/usfsctl status
