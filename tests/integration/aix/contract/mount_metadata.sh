#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Reject malformed AIX vmount metadata without publishing a mount, then
#   prove a valid scenario can mount and serve data afterward.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/mount_metadata.sh
#
# Notes:
#   Corruption is injected by the test-only scenario daemon. Each failed mount
#   is bounded, reaped, and checked for absence from the AIX mount table.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-mount-metadata.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

corruption_case()
{
    corruption=$1
    description=$2
    failure=0

    rm -f "$REQUESTS" "$DAEMON_LOG"
    USFS_SCENARIO_VMOUNT_CORRUPTION="$corruption" \
        /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"

    elapsed=0
    while kill -0 "$DAEMON_PID" 2>/dev/null &&
          [ "$elapsed" -lt 10 ]; do
        if is_mounted; then
            failure=1
            break
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    if kill -0 "$DAEMON_PID" 2>/dev/null; then
        failure=1
        kill -TERM "$DAEMON_PID" 2>/dev/null
    fi
    wait "$DAEMON_PID" 2>/dev/null
    daemon_rc=$?
    DAEMON_PID=0
    [ "$daemon_rc" -ne 0 ] || failure=1
    is_mounted && failure=1
    umount "$MOUNT" >/dev/null 2>&1 ||
        umount -f "$MOUNT" >/dev/null 2>&1

    tap_ok "$failure" "$description is rejected without publishing a mount"
}

normal_mount_recovers()
{
    failure=0

    rm -f "$REQUESTS" "$DAEMON_LOG"
    USFS_SCENARIO_SECOND_MOUNT="$BASE/second" /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "post-malformed mount recovery" 30 is_mounted ||
        failure=1
    if [ "$failure" -eq 0 ]; then
        content=$(cat "$MOUNT/file" 2>/dev/null)
        [ "$content" = "contract-data" ] || failure=1
    fi
    umount "$MOUNT" >/dev/null 2>&1 ||
        umount -f "$MOUNT" >/dev/null 2>&1
    kill -TERM "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0

    tap_ok "$failure" "valid mount succeeds after malformed metadata corpus"
    tap_ok $(grep -q SECOND_MOUNT_BUSY "$DAEMON_LOG" &&
             ! mount | grep -q " $BASE/second "; echo $?) \
        "same connection rejects a second mount before publication"
}

readonly_mount()
{
    policy=$1
    failure=0
    rm -f "$REQUESTS" "$DAEMON_LOG"
    env "$policy=1" /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "read-only mount" 30 is_mounted || failure=1
    if [ "$failure" -eq 0 ]; then
        /usr/sbin/usfs_io_probe readonly-all "$MOUNT" >>"$DAEMON_LOG" 2>&1 || failure=1
        if grep -E '^(9|1[0-7]) ' "$REQUESTS"; then
            failure=1
        fi
    fi
    umount "$MOUNT" >/dev/null 2>&1 || failure=1
    kill -TERM "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
    [ "$failure" -eq 0 ] || cat "$DAEMON_LOG" "$REQUESTS"
    tap_ok "$failure" "$policy blocks every mutation while retaining readable data"
}

usfs_track_mount "$BASE/second"
trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT" "$BASE/second"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 11

corruption_case negative-info-offset "negative info offset"
corruption_case past-end-info "info range beyond vmount length"
corruption_case unterminated-info "unterminated info field"
corruption_case overflow-cookie "overflowing cookie"
corruption_case invalid-rw "invalid writable flag"
corruption_case missing-rw "missing writable policy"
normal_mount_recovers
race_failure=0
USFS_SCENARIO_RACE_MOUNT="$BASE/second" /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"
usfs_wait_until "simultaneous mount result" 15 grep -q '^READY ' "$DAEMON_LOG" || race_failure=1
winner=$(sed -n 's/^MOUNT_RACE_WINNER //p' "$DAEMON_LOG")
case "$winner" in
    "$MOUNT"|"$BASE/second")
        /usr/sbin/usfs_io_probe stat "$winner/file" >>"$DAEMON_LOG" 2>&1 || race_failure=1 ;;
    *) race_failure=1 ;;
esac
count=$(mount | awk -v a="$MOUNT" -v b="$BASE/second" '$2==a || $2==b {n++} END {print n+0}')
[ "$count" -eq 1 ] || race_failure=1
if [ -n "$winner" ]; then
    case "$winner" in "$MOUNT"|"$BASE/second") umount "$winner" || race_failure=1 ;; esac
fi
kill -TERM "$DAEMON_PID" 2>/dev/null
wait "$DAEMON_PID" 2>/dev/null || race_failure=1
DAEMON_PID=0
[ "$race_failure" -eq 0 ] || cat "$DAEMON_LOG"
tap_ok "$race_failure" "simultaneous mount attempts publish exactly one usable mount and return EBUSY to the other"

readonly_mount USFS_SCENARIO_READONLY_FLAGS
readonly_mount USFS_SCENARIO_READONLY_POLICY

tap_finish
