#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Prove a one-shot pincode failure leaves no partial configuration and that
#   an immediate clean CFG_INIT retry restores the testing control device.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/fault/init_retry.sh
#
# Notes:
#   Temporarily unconfigures usfs0 and preserves coverage before unload when
#   USFS_COVERAGE_SUITE is set. The final state must be Available and usable.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

tap_plan 5

if ! usfs_coverage_checkpoint init-retry-pre-term; then
    tap_diag "unable to preserve coverage before initialization retry"
    exit 1
fi

usfs_coverage_expect_export init-retry-term || exit 1
if USFS_COVERAGE_EVENT=init-retry-term \
   rmdev -l usfs0 >/tmp/usfs_fault_rmdev.log 2>&1; then
    tap_ok 0 "test device unconfigured"
else
    cat /tmp/usfs_fault_rmdev.log | while IFS= read -r line; do tap_diag "$line"; done
    tap_ok 1 "test device unconfigured"
fi

usfs_coverage_expect_export init-retry-pin-failure || exit 1
if USFS_COVERAGE_EVENT=init-retry-pin-failure \
   USFS_TEST_INIT_FAULT=1 USFS_TEST_INIT_ERRNO=12 \
   USFS_TEST_INIT_OCCURRENCE=1 mkdev -l usfs0 \
   >/tmp/usfs_fault_init.log 2>&1; then
    tap_diag "CFG_INIT unexpectedly succeeded with pincode fault armed"
    tap_ok 1 "injected initialization failure returned an error"
else
    tap_ok 0 "injected initialization failure returned an error"
fi

if mkdev -l usfs0 >/tmp/usfs_fault_retry.log 2>&1; then
    tap_ok 0 "configuration succeeds on retry"
else
    cat /tmp/usfs_fault_retry.log | while IFS= read -r line; do tap_diag "$line"; done
    tap_ok 1 "configuration succeeds on retry"
fi

if /usr/sbin/usfs_testctl info >/tmp/usfs_fault_info.log 2>&1; then
    tap_ok 0 "test control is usable after retry"
else
    cat /tmp/usfs_fault_info.log | while IFS= read -r line; do tap_diag "$line"; done
    tap_ok 1 "test control is usable after retry"
fi

log_race_failure=0
if [ -x /usr/sbin/usfs_coverage ]; then
    tap_ok 0 "concurrent configuration teardown is serialized # SKIP coverage unload accounting"
else
    cycle=1
    while [ "$cycle" -le 8 ]; do
        rmdev -l usfs0 >"/tmp/usfs_log_race.$$.${cycle}.1" 2>&1 &
        first=$!
        rmdev -l usfs0 >"/tmp/usfs_log_race.$$.${cycle}.2" 2>&1 &
        second=$!
        wait "$first" 2>/dev/null
        wait "$second" 2>/dev/null

        if [ "$(lsdev -l usfs0 -F status 2>/dev/null)" != "Defined" ] ||
           ! mkdev -l usfs0 >/dev/null 2>&1 ||
           ! /usr/sbin/usfs_testctl info >/dev/null 2>&1; then
            log_race_failure=1
            break
        fi
        cycle=$((cycle + 1))
    done
    rm -f /tmp/usfs_log_race.$$.*
    tap_ok "$log_race_failure" "concurrent configuration teardown is serialized"
fi

tap_finish
