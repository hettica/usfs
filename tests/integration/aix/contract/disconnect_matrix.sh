#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Validate daemon disconnect handling for idle, pending, delivered, mixed,
#   canceled, late-reply, and reply-copy ownership states.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/disconnect_matrix.sh
#
# Notes:
#   Requires USFS_TESTING and the scenario daemon. All waits are bounded; each
#   case proves fresh-channel recovery and final mount/process quiescence.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-disconnect-matrix.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
BARRIERS=$BASE/barriers
DAEMON_PID=0
READER_PIDS=""

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

daemon_exited()
{
    ! ps -p "$DAEMON_PID" >/dev/null 2>&1
}

start_daemon()
{
    pause_before_read=${1:-0}
    rm -f "$REQUESTS" "$DAEMON_LOG"
    rm -rf "$BARRIERS"
    mkdir -p "$BARRIERS"
    if [ "$pause_before_read" -eq 1 ]; then
        USFS_SCENARIO_BARRIER_DIR="$BARRIERS" \
        USFS_SCENARIO_PAUSE_BEFORE_READ=1 \
            /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
            >"$DAEMON_LOG" 2>&1 &
    else
        USFS_SCENARIO_BARRIER_DIR="$BARRIERS" \
            /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
            >"$DAEMON_LOG" 2>&1 &
    fi
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "disconnect-matrix mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

kill_daemon()
{
    kill -TERM "$DAEMON_PID" 2>/dev/null
    usfs_wait_until "scenario daemon exit" 15 daemon_exited
}

spawn_reader()
{
    index=$1
    (
        cat "$MOUNT/file" >"$BASE/reader.$index.out" \
            2>"$BASE/reader.$index.err"
        printf '%s\n' "$?" >"$BASE/reader.$index.rc"
    ) &
    pid=$!
    READER_PIDS="$READER_PIDS $pid"
    usfs_track_pid "$pid"
}

readers_done()
{
    expected=$1
    count=$(find "$BASE" -name 'reader.*.rc' -print 2>/dev/null | wc -l)
    [ "$count" -eq "$expected" ]
}

wait_readers_failed()
{
    expected=$1
    failures=0

    if ! usfs_wait_until "$expected filesystem callers" 15 \
         readers_done "$expected"; then
        return 1
    fi
    for pid in $READER_PIDS; do
        wait "$pid" 2>/dev/null
    done
    index=1
    while [ "$index" -le "$expected" ]; do
        rc=$(cat "$BASE/reader.$index.rc" 2>/dev/null)
        case "$rc" in
            ""|*[!0-9]*) failures=$((failures + 1)) ;;
            0) failures=$((failures + 1)) ;;
        esac
        index=$((index + 1))
    done
    [ "$failures" -eq 0 ]
}

reset_readers()
{
    READER_PIDS=""
    rm -f "$BASE"/reader.*.out "$BASE"/reader.*.err "$BASE"/reader.*.rc
}

recovery_works()
{
    printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
    if ! start_daemon 0; then
        stop_daemon
        return 1
    fi
    content=$(cat "$MOUNT/file" 2>/dev/null)
    rc=$?
    stop_daemon
    [ "$rc" -eq 0 ] && [ "$content" = "contract-data" ]
}

no_usfs_state()
{
    mounts=$(mount 2>/dev/null | awk '$3 == "usfs" { n++ } END { print n+0 }')
    daemons=$(ps -ef | grep -c '[u]sfs_scenario_daemon')
    [ "$mounts" -eq 0 ] && [ "$daemons" -eq 0 ]
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT" "$BARRIERS"
tap_plan 7

# Closing an idle daemon channel must make the still-mounted filesystem fail
# fast, and cleanup must permit an entirely new connection.
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
idle_failure=0
start_daemon 0 || idle_failure=1
kill_daemon || idle_failure=1
ls "$MOUNT/file" >/dev/null 2>&1 && idle_failure=1
stop_daemon
recovery_works || idle_failure=1
tap_ok "$idle_failure" "idle daemon close fails fast and a new channel recovers"

# Pause before the first read so all callers remain on the pending queue.
reset_readers
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
pending_failure=0
start_daemon 1 || pending_failure=1
usfs_wait_until "before-read barrier" 10 \
    test -f "$BARRIERS/before-read.ready" || pending_failure=1
index=1
while [ "$index" -le 8 ]; do
    spawn_reader "$index"
    index=$((index + 1))
done
sleep 1
kill_daemon || pending_failure=1
wait_readers_failed 8 || pending_failure=1
stop_daemon
recovery_works || pending_failure=1
tap_ok "$pending_failure" "disconnect aborts eight pending callers exactly once"

# Hold after read() returns: the kernel request is delivered and waits for its
# reply on the processing list.
reset_readers
printf 'USFS-SCENARIO 1\nLOOKUP hold 30000 1\n' >"$SCENARIO"
sent_failure=0
start_daemon 0 || sent_failure=1
spawn_reader 1
usfs_wait_until "after-read barrier" 10 \
    test -f "$BARRIERS/after-read.ready" || sent_failure=1
kill_daemon || sent_failure=1
wait_readers_failed 1 || sent_failure=1
stop_daemon
recovery_works || sent_failure=1
tap_ok "$sent_failure" "disconnect aborts a delivered unanswered request"

# One delivered request blocks the daemon while later requests accumulate in
# the pending queue; close must wake every owner without double completion.
reset_readers
printf 'USFS-SCENARIO 1\nLOOKUP hold 30000 1\n' >"$SCENARIO"
mixed_failure=0
start_daemon 0 || mixed_failure=1
spawn_reader 1
usfs_wait_until "mixed after-read barrier" 10 \
    test -f "$BARRIERS/after-read.ready" || mixed_failure=1
index=2
while [ "$index" -le 8 ]; do
    spawn_reader "$index"
    index=$((index + 1))
done
sleep 1
kill_daemon || mixed_failure=1
wait_readers_failed 8 || mixed_failure=1
stop_daemon
recovery_works || mixed_failure=1
tap_ok "$mixed_failure" "disconnect aborts mixed delivered and pending callers"

# Signal a delivered waiter, then allow its reply before process teardown.
# Completion must preserve the channel and release the request's queue slot.
printf 'USFS-SCENARIO 1\nLOOKUP hold 30000 1\n' >"$SCENARIO"
cancel_failure=0
start_daemon 0 || cancel_failure=1
cat "$MOUNT/file" >"$BASE/canceled.out" 2>"$BASE/canceled.err" &
CANCELED_PID=$!
usfs_track_pid "$CANCELED_PID"
usfs_wait_until "cancellation after-read barrier" 10 \
    test -f "$BARRIERS/after-read.ready" || cancel_failure=1
kill -TERM "$CANCELED_PID" 2>/dev/null
touch "$BARRIERS/after-read.release"
wait "$CANCELED_PID" 2>/dev/null
[ "$?" -ne 0 ] || cancel_failure=1
usfs_wait_until "late reply completion" 10 \
    grep -q '^REPLIED ' "$REQUESTS" || cancel_failure=1
content=$(cat "$MOUNT/file" 2>/dev/null)
[ "$?" -eq 0 ] && [ "$content" = "contract-data" ] || cancel_failure=1
stop_daemon
recovery_works || cancel_failure=1
tap_ok "$cancel_failure" "delivered reply completes before signaled caller teardown and preserves channel"

# The existing test-only reply-copy fault exercises the REPLYING ownership
# path. The daemon exits after write(2) fails; the waiter must wake and a new
# connection must remain possible.
reply_failure=0
/usr/sbin/usfs_testctl arm 14 1 14 >/dev/null 2>&1 || reply_failure=1
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
start_daemon 0 || reply_failure=1
cat "$MOUNT/file" >/dev/null 2>&1 && reply_failure=1
usfs_wait_until "reply-copy daemon exit" 15 daemon_exited || reply_failure=1
stop_daemon
recovery_works || reply_failure=1
tap_ok "$reply_failure" "reply-copy failure and daemon close release ownership"

final_failure=0
no_usfs_state || final_failure=1
/usr/sbin/usfs_testctl info >/dev/null 2>&1 || final_failure=1
tap_ok "$final_failure" "disconnect matrix leaves a usable quiescent extension"

tap_finish
