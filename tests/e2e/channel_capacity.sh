#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify that status stays available when all backend device channels are open.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.

set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-channel-capacity.$$
NODE=/dev/usfs0
BACKEND_CHANNELS=64
CHANNEL_FDS=()
mkdir -p "$BASE" || exit 1

cleanup()
{
    for descriptor in "${CHANNEL_FDS[@]}"; do
        eval "exec ${descriptor}>&-"
    done
    rm -rf "$BASE"
}
trap cleanup EXIT HUP INT TERM

tap_plan 5

allocation_failed=0
for ((channel_index = 0; channel_index < BACKEND_CHANNELS; channel_index++)); do
    if ! exec {descriptor}<>"$NODE"; then
        allocation_failed=1
        break
    fi
    CHANNEL_FDS+=("$descriptor")
done
tap_ok "$allocation_failed" "all 64 backend channels can be opened"

status_failed=0
/usr/sbin/usfsctl status >"$BASE/full.status" 2>"$BASE/full.error" || status_failed=1
grep -q '^USFS health: HEALTHY$' "$BASE/full.status" || status_failed=1
grep -q '^daemons: 0 active, 0 unhealthy$' "$BASE/full.status" || status_failed=1
tap_ok "$status_failed" "status is readable at backend channel capacity"

extra_channel_accepted=0
if { exec {extra_descriptor}<>"$NODE"; } 2>"$BASE/extra.error"; then
    extra_channel_accepted=1
    eval "exec ${extra_descriptor}>&-"
fi
tap_ok "$extra_channel_accepted" "a 65th backend channel is still rejected"

status_failed=0
status_pids=()
for status_index in 1 2 3 4; do
    /usr/sbin/usfsctl status >"$BASE/status.$status_index" 2>&1 &
    status_pids+=("$!")
done
for status_pid in "${status_pids[@]}"; do
    wait "$status_pid" || status_failed=1
done
for status_index in 1 2 3 4; do
    grep -q '^USFS health: HEALTHY$' "$BASE/status.$status_index" || status_failed=1
done
tap_ok "$status_failed" "concurrent status queries do not need backend slots"

recovery_failed=0
if [ "${#CHANNEL_FDS[@]}" -gt 0 ]; then
    first_descriptor=${CHANNEL_FDS[0]}
    eval "exec ${first_descriptor}>&-"
    CHANNEL_FDS=("${CHANNEL_FDS[@]:1}")

    if exec {recovered_descriptor}<>"$NODE" 2>"$BASE/recovered.error"; then
        eval "exec ${recovered_descriptor}>&-"
    else
        recovery_failed=1
    fi
else
    recovery_failed=1
fi
tap_ok "$recovery_failed" "backend admission recovers when one slot is freed"

tap_finish
