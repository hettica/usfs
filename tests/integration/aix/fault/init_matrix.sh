#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Cover initialization and reverse-order cleanup fault points, proving every
#   failure is unwindable and that configuration/termination retries succeed.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/fault/init_matrix.sh
#
# Notes:
#   Temporarily runs rmdev/mkdev for usfs0. Under coverage, checkpoints preserve
#   counters before an unload; the script finishes with usfs0 Available.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

tap_plan 8

ensure_defined()
{
    event=$1
    usfs_coverage_checkpoint "$event-pre-term" || return 1
    usfs_coverage_expect_export "$event-term" || return 1
    USFS_COVERAGE_EVENT="$event-term" rmdev -l usfs0 >/dev/null 2>&1
    return 0
}

restore_available()
{
    mkdev -l usfs0 >/dev/null 2>&1
}

init_failure_case()
{
    fault=$1
    name=$2
    event="init-matrix-$fault"
    ensure_defined "$event"
    usfs_coverage_expect_export "$event-init-failure" || return
    if USFS_COVERAGE_EVENT="$event-init-failure" \
       USFS_TEST_INIT_FAULT="$fault" USFS_TEST_INIT_ERRNO=12 \
       USFS_TEST_INIT_OCCURRENCE=1 mkdev -l usfs0 >/tmp/usfs-init-matrix.log 2>&1; then
        tap_diag "$name unexpectedly configured"
        tap_ok 1 "$name failure unwinds and retries"
        return
    fi
    if restore_available; then
        tap_ok 0 "$name failure unwinds and retries"
    else
        sed 's/^/# /' /tmp/usfs-init-matrix.log
        tap_ok 1 "$name failure unwinds and retries"
    fi
}

cleanup_failure_case()
{
    fault=$1
    name=$2
    event="init-matrix-$fault"
    ensure_defined "$event"
    if ! USFS_TEST_INIT_FAULT="$fault" USFS_TEST_INIT_ERRNO=16 \
       USFS_TEST_INIT_OCCURRENCE=1 mkdev -l usfs0 >/tmp/usfs-cleanup-arm.log 2>&1; then
        tap_diag "$name could not be armed during configuration"
        restore_available
        tap_ok 1 "$name cleanup is retryable"
        return
    fi
    # A failed devswdel leaves the extension active, so there is no deferred
    # cleanup export until the successful termination retry.
    if [ "$fault" -ne 5 ]; then
        usfs_coverage_expect_export "$event-term-failure" || return
    fi
    if USFS_COVERAGE_EVENT="$event-term-failure" \
       rmdev -l usfs0 >/tmp/usfs-cleanup-fail.log 2>&1; then
        tap_diag "$name cleanup unexpectedly succeeded"
        restore_available
        tap_ok 1 "$name cleanup is retryable"
        return
    fi

    if [ ! -x /usr/sbin/usfs_coverage ] &&
       [ "$fault" -eq 5 ] && ! /usr/sbin/usfs_testctl info >/dev/null 2>&1; then
        tap_diag "failed devswdel did not restore device callback admission"
        restore_available
        tap_ok 1 "$name cleanup is retryable"
        return
    fi

    if [ ! -x /usr/sbin/usfs_coverage ] &&
       [ "$fault" -eq 6 ] && /usr/sbin/usfs_testctl info >/dev/null 2>&1; then
        tap_diag "failed GFS removal reopened a removed device registration"
        restore_available
        tap_ok 1 "$name cleanup is retryable"
        return
    fi

    if [ "$fault" -eq 5 ]; then
        usfs_coverage_checkpoint "$event-active-after-failed-term" || return
    fi

    usfs_coverage_expect_export "$event-term-retry" || return
    if USFS_COVERAGE_EVENT="$event-term-retry" \
       rmdev -l usfs0 >/tmp/usfs-cleanup-retry.log 2>&1 &&
       restore_available; then
        tap_ok 0 "$name cleanup is retryable"
    else
        sed 's/^/# /' /tmp/usfs-cleanup-retry.log
        tap_ok 1 "$name cleanup is retryable"
    fi
}

init_failure_case 1 "pincode"
if /usr/sbin/usfs_coverage info >/dev/null 2>&1; then
    init_failure_case 2 "coverage initialization"
else
    USFS_TAP_EXECUTED=$((USFS_TAP_EXECUTED + 1))
    echo "ok $USFS_TAP_EXECUTED - coverage initialization # SKIP non-coverage test build"
fi
init_failure_case 3 "GFS registration"
init_failure_case 4 "device registration"
cleanup_failure_case 5 "device unregistration"
cleanup_failure_case 6 "GFS unregistration"
cleanup_failure_case 7 "unpincode"

unwind_failure=0
ensure_defined "init-unwind" || unwind_failure=1
usfs_coverage_expect_export "init-unwind-failure" || unwind_failure=1
USFS_COVERAGE_EVENT=init-unwind-failure USFS_TEST_INIT_FAULT=18 \
    USFS_TEST_INIT_ERRNO=16 mkdev -l usfs0 >/tmp/usfs-init-unwind.log 2>&1 && unwind_failure=1
if [ -x /usr/sbin/usfs_coverage ]; then
    # The required coverage drain retries cleanup after capturing the failed
    # unwind; that successful drain may already have removed the registration.
    :
else
    [ -c /dev/usfs0 ] || unwind_failure=1
    owned_number=$(ls -ln /dev/usfs0 2>/dev/null | awk '{ print $5 $6 }')
    /usr/lib/methods/usfs/cfgusfs -l usfs0 >/dev/null 2>&1 && unwind_failure=1
    [ "$(ls -ln /dev/usfs0 2>/dev/null | awk '{ print $5 $6 }')" = "$owned_number" ] || unwind_failure=1
fi
/usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || unwind_failure=1
[ ! -e /dev/usfs0 ] || unwind_failure=1
restore_available || unwind_failure=1
[ "$unwind_failure" -eq 0 ] || sed 's/^/# /' /tmp/usfs-init-unwind.log
tap_ok "$unwind_failure" "failed initialization unwind retains ownership until cleanup of the original registration"

tap_finish
