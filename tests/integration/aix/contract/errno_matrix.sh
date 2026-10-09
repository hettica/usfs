#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Confirm that daemon errno replies for every wire opcode reach the matching
#   filesystem caller without corrupting later requests or lifecycle state.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/errno_matrix.sh
#
# Notes:
#   Requires the testing scenario daemon. Expected failing filesystem commands
#   can print AIX diagnostics while the TAP case itself still passes.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-errno-contract.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests
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
    usfs_wait_until "errno scenario mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

trigger()
{
    case "$1" in
        1) ls "$MOUNT/file" >/dev/null 2>&1 ;;
        2) ls -ld "$MOUNT/file" >/dev/null 2>&1 ;;
        3) cat "$MOUNT/file" >/dev/null 2>&1 ;;
        4) cat "$MOUNT/file" >/dev/null 2>&1 ;;
        5) cat "$MOUNT/file" >/dev/null 2>&1 ;;
        6) ls "$MOUNT" >/dev/null 2>&1 ;;
        7) perl -e 'exit !defined readlink($ARGV[0])' "$MOUNT/link" ;;
        8) /usr/sbin/usfs_io_probe statfs "$MOUNT" >"$BASE/statfs.log" 2>&1
           grep -q '^result=0 errno=0$' "$BASE/statfs.log" ;;
        9) printf x >"$MOUNT/new" 2>/dev/null ;;
        10) mkdir "$MOUNT/newdir" >/dev/null 2>&1 ;;
        11) rm "$MOUNT/file" >/dev/null 2>&1 ;;
        12) rmdir "$MOUNT/dir" >/dev/null 2>&1 ;;
        13) mv "$MOUNT/file" "$MOUNT/moved" >/dev/null 2>&1 ;;
        14) ln -s file "$MOUNT/newlink" >/dev/null 2>&1 ;;
        15) ln "$MOUNT/file" "$MOUNT/hard" >/dev/null 2>&1 ;;
        16) chmod 0600 "$MOUNT/file" >/dev/null 2>&1 ;;
        17) printf x >>"$MOUNT/file" 2>/dev/null ;;
        *) return 2 ;;
    esac
}

opcode_name()
{
    case "$1" in
        1) echo LOOKUP ;; 2) echo GETATTR ;; 3) echo OPEN ;;
        4) echo READ ;; 5) echo RELEASE ;; 6) echo READDIR ;;
        7) echo READLINK ;; 8) echo STATFS ;; 9) echo CREATE_ATTR ;;
        10) echo MKDIR ;; 11) echo UNLINK ;; 12) echo RMDIR ;;
        13) echo RENAME ;; 14) echo SYMLINK ;; 15) echo LINK ;;
        16) echo SETATTR ;; 17) echo WRITE ;;
    esac
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
tap_plan 21

opcode=1
while [ "$opcode" -le 17 ]; do
    name=$(opcode_name "$opcode")
    occurrence=1
    # Resolving the child now checks search permission on its parent. That
    # prerequisite obtains the root attributes, so target the child GETATTR
    # rather than the prerequisite root GETATTR during path traversal.
    [ "$opcode" -ne 2 ] || occurrence=2
    printf 'USFS-SCENARIO 1\n%s error 5 %s\n' \
        "$name" "$occurrence" >"$SCENARIO"
    failure=0
    if start_daemon; then
        trigger "$opcode"
        operation_rc=$?
        wire_opcode=$opcode
        [ "$opcode" -ne 9 ] || wire_opcode=21
        grep -q "^ACTION $wire_opcode 1 5$" "$REQUESTS" || failure=1
        # RELEASE errors are not returned by close(2).
        if [ "$opcode" -ne 5 ] &&
           [ "$operation_rc" -eq 0 ]; then
            failure=1
        fi
    else
        failure=1
    fi
    if [ "$failure" -ne 0 ]; then
        sed 's/^/# request: /' "$REQUESTS" 2>/dev/null
    fi
    stop_daemon
    tap_ok "$failure" "$name daemon errno is observed without residual state"
    opcode=$((opcode + 1))
done

for capacity_case in zero full invalid-statfs short-body; do
    failure=0
    printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
    case "$capacity_case" in
        zero|full) export USFS_SCENARIO_STATFS_CAPACITY=$capacity_case ;;
        *) unset USFS_SCENARIO_STATFS_CAPACITY
           printf 'STATFS %s 0 1\n' "$capacity_case" >>"$SCENARIO" ;;
    esac
    if start_daemon; then
        /usr/sbin/usfs_io_probe statfs "$MOUNT" >"$BASE/statfs.log" 2>&1 || failure=1
        case "$capacity_case" in
            zero) grep -q '^result=0 errno=0$' "$BASE/statfs.log" &&
                  grep -q '^blocks=0 free=0 available=0 files=0 ffree=0 bsize=4096$' "$BASE/statfs.log" || failure=1 ;;
            full) grep -q '^result=0 errno=0$' "$BASE/statfs.log" &&
                  grep -q '^blocks=1024 free=0 available=0 files=128 ffree=0 bsize=4096$' "$BASE/statfs.log" || failure=1 ;;
            *) grep -q '^result=-1 errno=5$' "$BASE/statfs.log" || failure=1
               grep -q '^blocks=' "$BASE/statfs.log" && failure=1 ;;
        esac
    else
        failure=1
    fi
    [ "$failure" -eq 0 ] || cat "$BASE/statfs.log" "$REQUESTS"
    stop_daemon
    tap_ok "$failure" "STATFS $capacity_case reports truthful capacity or failure and permits cleanup"
done
unset USFS_SCENARIO_STATFS_CAPACITY

tap_finish
