#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Verify the ABI and instrumentation boundary of a linked AIX kext.
# Usage: bash scripts/aix/verify-kernel.sh release|testing|coverage path

set -eu

variant=$1
kext=$2

test -s "$kext"
case "$variant" in
    release)
        ! nm -X64 -B "$kext" | grep -q 'usfs_test_'
        ;;
    testing)
        dump -X64 -H "$kext" | grep -q ' unix'
        test "$(dump -X64 -H "$kext" | grep -c ' unix')" -eq 1
        nm -X64 -B "$kext" | grep -q 'usfs_test_control_ioctl'
        ;;
    coverage)
        dump -X64 -H "$kext" | grep -q ' unix'
        test "$(dump -X64 -H "$kext" | grep -c ' unix')" -eq 1
        nm -X64 -B "$kext" | grep -q 'usfs_test_control_ioctl'
        nm -X64 -B "$kext" | grep -q 'usfs_test_kernel_selftest'
        ;;
    *)
        echo "unknown kernel variant: $variant" >&2
        exit 2
        ;;
esac
