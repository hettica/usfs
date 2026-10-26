#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Verify deterministic dispatch/unload schedules with a resident proxy.
# Usage: Run through make test on a testing or coverage AIX deployment.
set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"
tap_plan 12
tool=/usr/sbin/usfs_proxy_testctl
opener=0
terminator=0
status_fd_open=0
cleanup()
{
    "$tool" release >/dev/null 2>&1 || true
    if [[ "$opener" -ne 0 ]]; then wait "$opener" || true; fi
    if [[ "$status_fd_open" -ne 0 ]]; then exec 3<&-; fi
    if [[ "$terminator" -ne 0 ]]; then wait "$terminator" || true; fi
    mkdev -l usfs0 >/dev/null 2>&1 || true
}
trap cleanup EXIT
reached()
{
    local expected_point=$1

    [[ "$("$tool" query)" = "$expected_point" ]]
}
for point in 1 2 3; do
    if [[ "$point" -eq 1 ]]; then
        usfs_coverage_checkpoint proxy-pre-admission-pre-term || exit 1
        usfs_coverage_expect_export proxy-pre-admission-term || exit 1
    fi
    "$tool" arm "$point" || exit 1
    "$tool" open &
    opener=$!
    usfs_wait_until "proxy barrier $point" 30 reached "$point"
    tap_ok "$?" "proxy barrier $point reached explicitly"
    before=$(/usr/sbin/usfsctl kexts | sed -n '/^proxy:/p')
    if [[ "$point" -eq 1 ]]; then
        USFS_COVERAGE_EVENT=proxy-pre-admission-term \
            /usr/lib/methods/usfs/ucfgusfs -l usfs0
        tap_ok "$?" "pre-admission callback does not prevent implementation unload"
    else
        if /usr/lib/methods/usfs/ucfgusfs -l usfs0; then result=1; else result=0; fi
        tap_ok "$result" "admitted callback prevents implementation unload at point $point"
    fi
    "$tool" release || exit 1
    wait "$opener"; result=$?
    opener=0
    mkdev -l usfs0 >/dev/null || result=1
    after=$(/usr/sbin/usfsctl kexts | sed -n '/^proxy:/p')
    before_id=$(printf '%s\n' "$before" | sed 's/.*module=\([^ ]*\).*/\1/')
    after_id=$(printf '%s\n' "$after" | sed 's/.*module=\([^ ]*\).*/\1/')
    [[ "$before_id" = "$after_id" ]] || result=1
    tap_ok "$result" "callback completes safely and resident proxy identity survives point $point"
done

usfs_coverage_checkpoint proxy-closing-pre-term || exit 1
usfs_coverage_expect_export proxy-closing-term || exit 1
exec 3</dev/usfs0/status || exit 1
status_fd_open=1
"$tool" arm 4 || exit 1
USFS_COVERAGE_EVENT=proxy-closing-term \
    /usr/lib/methods/usfs/ucfgusfs -l usfs0 3<&- &
terminator=$!
usfs_wait_until "proxy closing barrier" 30 reached 4
result=$?
closing_status=$(/usr/sbin/usfsctl kexts) || result=1
printf '%s\n' "$closing_status" | grep -q '^proxy: .* state=3 ' || result=1
tap_ok "$result" 'termination reaches the proxy closing phase'
exec 3<&-
status_fd_open=0
"$tool" release || exit 1
wait "$terminator"; result=$?
terminator=0
tap_ok "$result" 'status channel deallocates while proxy admission is closing'
mkdev -l usfs0; result=$?
tap_ok "$result" 'device reconfigures after closing-phase channel drain'
tap_finish
