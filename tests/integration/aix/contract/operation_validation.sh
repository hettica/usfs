#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Feed malformed success bodies for operation-specific reply validators and
#   prove rejection is contained without poisoning the daemon channel.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/operation_validation.sh
#
# Notes:
#   Requires the test-only scenario daemon. Every malformed reply is followed
#   by recovery on the same channel, or a fresh channel for ownership faults.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-operation-validation.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

start_daemon()
{
    rm -f "$REQUESTS" "$DAEMON_LOG"
    /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "operation-validation mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

invoke_operation()
{
    opcode=$1

    case "$opcode" in
        LOOKUP) ls "$MOUNT/file" >/dev/null 2>&1 ;;
        GETATTR) ls -ld "$MOUNT/file" >/dev/null 2>&1 ;;
        CREATE_ATTR) : >"$MOUNT/newfile" ;;
        WRITE) printf 'x' >"$MOUNT/newfile" ;;
        READDIR) ls "$MOUNT" >/dev/null 2>&1 ;;
        STATFS) df "$MOUNT" >/dev/null 2>&1 ;;
        *) return 2 ;;
    esac
}

validation_case()
{
    opcode=$1
    action=$2
    expected_success=$3
    failure=0

    occurrence=1
    # Child GETATTR follows the root attribute lookup needed to authorize
    # search of the parent directory.
    [ "$opcode" != GETATTR ] || occurrence=2
    printf 'USFS-SCENARIO 1\n%s %s 0 %s\n' \
        "$opcode" "$action" "$occurrence" >"$SCENARIO"
    if ! start_daemon; then
        failure=1
    else
        invoke_operation "$opcode" 2>>"$DAEMON_LOG"
        operation_rc=$?
        if [ "$expected_success" -eq 1 ]; then
            [ "$operation_rc" -eq 0 ] || failure=1
        else
            [ "$operation_rc" -ne 0 ] || failure=1
        fi
        grep -q "^ACTION " "$REQUESTS" 2>/dev/null || failure=1
        content=$(cat "$MOUNT/file" 2>/dev/null)
        case "$opcode" in
            LOOKUP|CREATE_ATTR)
                [ -z "$content" ] || failure=1
                stop_daemon
                printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
                start_daemon || failure=1
                content=$(cat "$MOUNT/file" 2>/dev/null)
                ;;
        esac
        [ "$content" = "contract-data" ] || failure=1
    fi
    stop_daemon
    tap_ok "$failure" "$4 is contained and the channel recovers"
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
tap_plan 9

validation_case LOOKUP invalid-nodeid 0 "zero lookup node id"
validation_case LOOKUP invalid-attr 0 "invalid lookup attributes"
validation_case GETATTR invalid-parent 0 "zero attribute parent"
validation_case CREATE_ATTR invalid-nodeid 0 "zero create node id"
validation_case WRITE invalid-write-count 0 "write count beyond request"
validation_case WRITE invalid-write-offset 0 "overflowing write result offset"
validation_case READDIR invalid-readdir-count 0 "readdir count beyond records"
validation_case READDIR invalid-readdir-record 0 "malformed readdir record"
validation_case STATFS invalid-statfs 0 "invalid statfs failure"

tap_finish
