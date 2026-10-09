#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Inject each runtime allocation/copy failure once and prove the caller,
#   accounting state, daemon channel, and subsequent operation recover.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/fault/allocation_matrix.sh
#
# Notes:
#   Requires USFS_TESTING fault controls. Faults may be armed only while the
#   extension is quiescent, and each case verifies exactly one firing.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-allocation-matrix.$$
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
    usfs_wait_until "allocation-matrix mount" 30 is_mounted
}

wait_for_daemon_outcome()
{
    elapsed=0

    while [ "$elapsed" -lt 30 ]; do
        if is_mounted; then
            return 0
        fi
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            wait "$DAEMON_PID" 2>/dev/null
            DAEMON_PID=0
            return 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 2
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

status_fired()
{
    /usr/sbin/usfs_testctl status 2>/dev/null |
        grep -q "observed=$1 fired=1 armed=0"
}

allocation_case()
{
    fault=$1
    name=$2
    failure=0
    occurrence=1

    # Request-buffer faults on the root GETATTR are intentionally hidden by
    # the root stat fallback. Target the following LOOKUP so cat observes the
    # injected allocation failure.
    case "$fault" in
        8|9|10|17) occurrence=2 ;;
    esac

    /usr/sbin/usfs_testctl arm "$fault" "$occurrence" 12 >/dev/null 2>&1 || failure=1
    if [ "$failure" -eq 0 ]; then
        rm -f "$REQUESTS" "$DAEMON_LOG"
        /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
            >"$DAEMON_LOG" 2>&1 &
        DAEMON_PID=$!
        usfs_track_pid "$DAEMON_PID"
        usfs_track_mount "$MOUNT"

        wait_for_daemon_outcome
        outcome=$?
        if [ "$outcome" -eq 0 ]; then
            cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
        elif [ "$outcome" -eq 2 ]; then
            failure=1
        fi
    fi
    stop_daemon
    status_fired "$occurrence" || failure=1
    start_daemon || failure=1
    if [ "$failure" -eq 0 ]; then
        content=$(cat "$MOUNT/file" 2>/dev/null)
        [ "$content" = "contract-data" ] || failure=1
    fi
    stop_daemon
    tap_ok "$failure" "$name fails once, records firing, and recovers"
}

write_copy_case()
{
    failure=0

    /usr/sbin/usfs_testctl arm 15 1 14 >/dev/null 2>&1 || failure=1
    start_daemon || failure=1
    if [ "$failure" -eq 0 ]; then
        printf 'first' >"$MOUNT/newfile" 2>/dev/null && failure=1
    fi
    stop_daemon
    status_fired 1 || failure=1

    start_daemon || failure=1
    if [ "$failure" -eq 0 ]; then
        printf 'second' >"$MOUNT/newfile" 2>/dev/null || failure=1
    fi
    stop_daemon

    tap_ok "$failure" \
        "write input copy failure preserves accounting and the next write recovers"
}

create_atomic_case()
{
    failure=0
    create_rc=0
    create_seen=0
    release_seen=0
    recovery_rc=0
    request_trace=''

    # Root construction is occurrence 1. The child allocation must now happen
    # before CREATE is delivered, so occurrence 2 cannot leave a namespace
    # side effect or a daemon handle requiring RELEASE.
    printf 'USFS-SCENARIO 1\nLOOKUP error 2 1\n' >"$SCENARIO"
    /usr/sbin/usfs_testctl arm 12 2 12 >/dev/null 2>&1 || failure=1
    start_daemon || failure=1
    if [ "$failure" -eq 0 ]; then
        ( : >"$MOUNT/create-cleanup" ) 2>/dev/null
        create_rc=$?
        [ "$create_rc" -ne 0 ] || failure=1
        grep -q '^9 ' "$REQUESTS" 2>/dev/null && create_seen=1
        grep -q '^5 ' "$REQUESTS" 2>/dev/null && release_seen=1
        [ "$create_seen" -eq 0 ] || failure=1
        [ "$release_seen" -eq 0 ] || failure=1
        request_trace=$(awk '!/^REPLIED / { printf "%s%s", separator, $0; separator="; " }' "$REQUESTS" 2>/dev/null)
    fi
    stop_daemon
    status_fired 2 || failure=1

    start_daemon || failure=1
    if [ "$failure" -eq 0 ]; then
        : >"$MOUNT/create-recovery" 2>/dev/null
        recovery_rc=$?
        [ "$recovery_rc" -eq 0 ] || failure=1
    fi
    stop_daemon

    if [ "$failure" -ne 0 ]; then
        tap_diag "create_rc=$create_rc create_seen=$create_seen release_seen=$release_seen recovery_rc=$recovery_rc"
        tap_diag "fault status: $(/usr/sbin/usfs_testctl status 2>/dev/null)"
        tap_diag "first request trace: $request_trace"
    fi
    tap_ok "$failure" \
        "failed local CREATE allocation occurs before daemon commit and recovers"
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 10

allocation_case 8 "request structure allocation"
allocation_case 9 "request message allocation"
allocation_case 10 "request reply allocation"
allocation_case 11 "gnode allocation"
allocation_case 12 "private node allocation"
allocation_case 14 "daemon reply copy"
allocation_case 16 "per-open state allocation"
allocation_case 17 "pinned request wait-context allocation"
write_copy_case
create_atomic_case

tap_finish
