#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Perform idempotent USFS device setup on the AIX VM: ensure the ODM
#   predefined entry exists, define usfs0, and configure the device so the
#   kernel extension is loaded and /dev/usfs0 is present.
#
# Usage:
#   Run from the repository root on the AIX VM:
#     bash scripts/aix/setup.sh
#
#   Public CMake targets call this helper while activating a build variant.

cd "$(dirname "$0")/../.." || exit 1

if ! odmget -q "uniquetype=cdr/fs/usfs" PdDv 2>/dev/null | grep -q usfs; then
    echo "== adding PdDv entry"
    odmadd packaging/aix/odm/usfs.pddv.add || exit 1
fi

# Note: AIX lsdev exits 0 even when the device does not exist (the error goes
# to stderr), so test its stdout instead of its exit code.
if ! lsdev -l usfs0 2>/dev/null | grep -q "^usfs0"; then
    echo "== defining usfs0"
    /usr/lib/methods/define -c cdr -s fs -t usfs -n -u || exit 1
fi

if ! lsdev -l usfs0 2>/dev/null | grep -q Available; then
    echo "== configuring usfs0"
    mkdev -l usfs0 || exit 1
fi

lsdev -l usfs0
ls -l /dev/usfs0
