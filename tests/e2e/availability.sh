#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise request bounds, health transition, backpressure, live chusfs
#   updates, and the pager-specific deadline through real filesystem calls.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/e2e/availability.sh

set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-availability.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
BARRIERS=$BASE/barriers
CHUSFS=/usr/lib/methods/usfs/chusfs
DAEMON_PID=0
PROBE_PIDS=""

is_mounted() { mount 2>/dev/null | grep -q " $MOUNT "; }
probe_done() { test -f "$1"; }

start_daemon()
{
    file_size=${1:-8192}
    rm -f "$REQUESTS" "$DAEMON_LOG"
    rm -rf "$BARRIERS"
    mkdir -p "$BARRIERS"
    USFS_SCENARIO_FULL_READS=1 \
    USFS_SCENARIO_FILE_SIZE="$file_size" \
    USFS_SCENARIO_BARRIER_DIR="$BARRIERS" \
        /usr/sbin/usfs_scenario_daemon \
        "$MOUNT" "$SCENARIO" "$REQUESTS" >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "availability mount" 30 is_mounted
}

stop_daemon()
{
    touch "$BARRIERS/after-read.release" 2>/dev/null
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

spawn_stat()
{
    index=$1
    rm -f "$BASE/stat.$index.out" "$BASE/stat.$index.rc"
    (
        /usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/stat.$index.out" 2>&1
        printf '%s\n' "$?" >"$BASE/stat.$index.rc"
    ) &
    pid=$!
    PROBE_PIDS="$PROBE_PIDS $pid"
    usfs_track_pid "$pid"
}

wait_probes()
{
    for pid in $PROBE_PIDS; do wait "$pid" 2>/dev/null; done
    PROBE_PIDS=""
}

restore_policy()
{
    "$CHUSFS" -l usfs0 \
        -a request_timeout_ms=30000 \
        -a pager_timeout_ms=10000 \
        -a max_outstanding_requests=256 >/dev/null 2>&1
}

recovery_works()
{
    printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
    start_daemon 8192 || return 1
    /usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/recovery.out" 2>&1
    recovery_rc=$?
    stop_daemon
    [ "$recovery_rc" -eq 0 ] && grep -q 'result=0 errno=0' "$BASE/recovery.out"
}

no_usfs_state()
{
    mounts=$(mount 2>/dev/null | awk '$3 == "usfs" { n++ } END { print n+0 }')
    daemons=$(ps -ef | grep -c '[u]sfs_scenario_daemon')
    [ "$mounts" -eq 0 ] && [ "$daemons" -eq 0 ]
}

trap 'stop_daemon; usfs_owned_cleanup; restore_policy; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT" "$BARRIERS"
tap_plan 10

config_failure=0
restore_policy || config_failure=1
before=$("$CHUSFS" -l usfs0 2>/dev/null) || config_failure=1
"$CHUSFS" -l usfs0 -a request_timeout_ms=99 >/dev/null 2>&1 && config_failure=1
su nobody -c "$CHUSFS -l usfs0 -a request_timeout_ms=1000" \
    >/dev/null 2>&1 && config_failure=1
after=$("$CHUSFS" -l usfs0 2>/dev/null) || config_failure=1
[ "$before" = "$after" ] || config_failure=1
echo "$after" | grep -q '^request_timeout_ms=30000$' || config_failure=1
echo "$after" | grep -q '^pager_timeout_ms=10000$' || config_failure=1
echo "$after" | grep -q '^max_outstanding_requests=256$' || config_failure=1
tap_ok "$config_failure" "chusfs reports policy and rejects invalid or unprivileged updates"

cleanup_failure=0
printf 'USFS-SCENARIO 1\nLOOKUP hold 30000 2\n' >"$SCENARIO"
start_daemon 8192 || cleanup_failure=1
"$CHUSFS" -l usfs0 -a max_outstanding_requests=1 >/dev/null || cleanup_failure=1
(
    /usr/sbin/usfs_io_probe held-close "$MOUNT/file" "$BASE/close.ready" "$BASE/close.release" \
        >"$BASE/close.out" 2>&1
    echo "$?" >"$BASE/close.rc"
) &
close_pid=$!
usfs_track_pid "$close_pid"
usfs_wait_until "preopened cleanup handle" 10 test -f "$BASE/close.ready" || cleanup_failure=1
spawn_stat cleanup
usfs_wait_until "ordinary slot occupied" 10 test -f "$BARRIERS/after-read.ready" || cleanup_failure=1
touch "$BASE/close.release"
reserve_observed=0
for attempt in 1 2 3 4 5; do
    health=$("$CHUSFS" -l usfs0 2>/dev/null)
    if echo "$health" | grep -q '^outstanding_requests=2$'; then reserve_observed=1; break; fi
    sleep 1
done
[ "$reserve_observed" -eq 1 ] || cleanup_failure=1
echo "$health" | grep -q '^active_connections=1$' || cleanup_failure=1
touch "$BARRIERS/after-read.release"
usfs_wait_until "reserved RELEASE completed" 10 test -f "$BASE/close.rc" || cleanup_failure=1
usfs_wait_until "ordinary waiter completed" 10 probe_done "$BASE/stat.cleanup.rc" || cleanup_failure=1
wait "$close_pid" || cleanup_failure=1
wait_probes
[ "$(cat "$BASE/close.rc" 2>/dev/null)" = 0 ] || cleanup_failure=1
/usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/after-cleanup.out" 2>&1 || cleanup_failure=1
grep -q 'result=0 errno=0' "$BASE/after-cleanup.out" || cleanup_failure=1
if [ "$cleanup_failure" -ne 0 ]; then
    tap_diag "cleanup reserve health: $health"
    tap_diag "cleanup close: $(cat "$BASE/close.out" "$BASE/close.rc" 2>/dev/null)"
    tap_diag "cleanup stat: $(cat "$BASE/stat.cleanup.out" "$BASE/after-cleanup.out" 2>/dev/null)"
    tap_diag "cleanup requests: $(cat "$REQUESTS" 2>/dev/null)"
fi
stop_daemon
restore_policy || cleanup_failure=1
tap_ok "$cleanup_failure" "ordinary saturation preserves one cleanup exchange and a healthy mount"

printf 'USFS-SCENARIO 1\nLOOKUP hold 30000 1\n' >"$SCENARIO"
queue_failure=0
start_daemon 8192 || queue_failure=1
policy=$("$CHUSFS" -l usfs0 \
    -a request_timeout_ms=1500 \
    -a pager_timeout_ms=500 \
    -a max_outstanding_requests=2 2>/dev/null) || queue_failure=1
echo "$policy" | grep -q '^active_connections=1$' || queue_failure=1
spawn_stat 1
usfs_wait_until "held availability lookup" 10 \
    test -f "$BARRIERS/after-read.ready" || queue_failure=1
spawn_stat 2
sleep 1
/usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/stat.3.out" 2>&1
grep -q 'result=-1 errno=11' "$BASE/stat.3.out" || queue_failure=1
tap_ok "$queue_failure" "per-connection queue limit rejects excess work with EAGAIN"

timeout_failure=0
usfs_wait_until "timed request completion" 8 probe_done "$BASE/stat.1.rc" || timeout_failure=1
usfs_wait_until "aborted sibling completion" 8 probe_done "$BASE/stat.2.rc" || timeout_failure=1
wait_probes
cat "$BASE/stat.1.out" "$BASE/stat.2.out" >"$BASE/stat.timeout.out"
grep -q 'result=-1 errno=78' "$BASE/stat.timeout.out" || timeout_failure=1
grep -q 'result=-1 errno=5' "$BASE/stat.timeout.out" || timeout_failure=1
health=$("$CHUSFS" -l usfs0 2>/dev/null) || timeout_failure=1
echo "$health" | grep -q '^active_connections=0$' || timeout_failure=1
/usr/sbin/usfsctl >"$BASE/status.timeout.out" 2>"$BASE/status.timeout.err"
[ "$?" -eq 1 ] || timeout_failure=1
grep -q '^USFS health: DEGRADED$' "$BASE/status.timeout.out" || timeout_failure=1
grep -q '^requests: .* [1-9][0-9]* timed out$' "$BASE/status.timeout.out" || timeout_failure=1
grep -q '^last fault: timeout ' "$BASE/status.timeout.out" || timeout_failure=1
/usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/stat.after.out" 2>&1
grep -q 'result=-1 errno=5' "$BASE/stat.after.out" || timeout_failure=1
[ "$timeout_failure" -eq 0 ] || {
    tap_diag "timeout outputs: $(cat "$BASE/stat.timeout.out" 2>/dev/null)"
    tap_diag "health: $health"
    tap_diag "status: $(cat "$BASE/status.timeout.out" 2>/dev/null)"
    tap_diag "post-timeout: $(cat "$BASE/stat.after.out" 2>/dev/null)"
}
tap_ok "$timeout_failure" "request timeout quarantines the mount and wakes queued callers"
stop_daemon

cached_failure=0
printf 'USFS-SCENARIO 1\nSETATTR hold 30000 1\n' >"$SCENARIO"
USFS_SCENARIO_COMMIT_SETATTR_BEFORE_HOLD=1 start_daemon 8192 || cached_failure=1
"$CHUSFS" -l usfs0 -a request_timeout_ms=1500 -a pager_timeout_ms=10000 \
    -a max_outstanding_requests=256 >/dev/null 2>&1 || cached_failure=1
(
    /usr/sbin/usfs_io_probe cached-quarantine "$MOUNT/file" >"$BASE/cached.out" 2>&1
    printf '%s\n' "$?" >"$BASE/cached.rc"
) &
cached_pid=$!
usfs_track_pid "$cached_pid"
usfs_wait_until "cached reads after committed truncate timeout" 20 \
    test -f "$BASE/cached.rc" || cached_failure=1
wait "$cached_pid" 2>/dev/null
[ "$(cat "$BASE/cached.rc" 2>/dev/null)" = 0 ] || cached_failure=1
grep -q '^COMMITTED_SIZE 0$' "$REQUESTS" || cached_failure=1
[ "$cached_failure" -eq 0 ] || sed 's/^/# /' "$BASE/cached.out"
stop_daemon
tap_ok "$cached_failure" "cached data and cached EOF reject reads after a committed truncate times out"

recovery_failure=0
recovery_works || recovery_failure=1
tap_ok "$recovery_failure" "fresh daemon channel recovers after timeout quarantine"

pager_failure=0
printf 'USFS-SCENARIO 1\nREAD hold 30000 1\n' >"$SCENARIO"
start_daemon 8192 || pager_failure=1
"$CHUSFS" -l usfs0 \
    -a request_timeout_ms=5000 \
    -a pager_timeout_ms=500 \
    -a max_outstanding_requests=256 >/dev/null 2>&1 || pager_failure=1
rm -f "$BASE/pager.out" "$BASE/pager.rc"
(
    /usr/sbin/usfs_io_probe mmap-touch-read \
        "$MOUNT/file" 4096 0 7 >"$BASE/pager.out" 2>&1
    printf '%s\n' "$?" >"$BASE/pager.rc"
) &
PAGER_PID=$!
usfs_track_pid "$PAGER_PID"
usfs_wait_until "held pager request" 10 \
    test -f "$BARRIERS/after-read.ready" || pager_failure=1
usfs_wait_until "bounded pager completion" 8 probe_done "$BASE/pager.rc" || pager_failure=1
wait "$PAGER_PID" 2>/dev/null
pager_rc=$(cat "$BASE/pager.rc" 2>/dev/null)
if ! grep -q 'result=-1' "$BASE/pager.out" 2>/dev/null; then
    case "$pager_rc" in
        ""|0|*[!0-9]*) pager_failure=1 ;;
    esac
fi
[ "$pager_failure" -eq 0 ] || {
    tap_diag "pager rc=$pager_rc output=$(cat "$BASE/pager.out" 2>/dev/null)"
    tap_diag "pager requests: $(cat "$REQUESTS" 2>/dev/null)"
}
tap_ok "$pager_failure" "pager request uses its shorter deadline and faults locally"
stop_daemon

for completion in reply timeout; do
    signal_failure=0
    printf 'USFS-SCENARIO 1\nREAD hold 30000 1\n' >"$SCENARIO"
    start_daemon 8192 || signal_failure=1
    "$CHUSFS" -l usfs0 -a request_timeout_ms=4000 \
        -a max_outstanding_requests=1 >/dev/null 2>&1 || signal_failure=1
    /usr/sbin/usfs_io_probe signaled-read "$MOUNT/file" \
        >"$BASE/signal.out" 2>&1 &
    signal_pid=$!
    usfs_track_pid "$signal_pid"
    usfs_wait_until "delivered read before repeated signals" 10 \
        test -f "$BARRIERS/after-read.ready" || signal_failure=1
    kill -USR1 "$signal_pid" || signal_failure=1
    kill -USR1 "$signal_pid" || signal_failure=1
    kill -USR1 "$signal_pid" || signal_failure=1
    /usr/sbin/usfs_io_probe stat "$MOUNT/dir" >"$BASE/signal.excess.out" 2>&1
    grep -q '^result=-1 errno=11$' "$BASE/signal.excess.out" || signal_failure=1
    if [ "$completion" = reply ]; then
        touch "$BARRIERS/after-read.release"
        wait "$signal_pid" || signal_failure=1
        grep -q '^result=0 errno=0 signal=1$' "$BASE/signal.out" || signal_failure=1
        /usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/signal.next.out" 2>&1
        grep -q '^result=0 errno=0$' "$BASE/signal.next.out" || signal_failure=1
    else
        usfs_wait_until "original deadline after signals" 8 \
            grep -q '^result=-1 errno=78 signal=1$' "$BASE/signal.out" || signal_failure=1
        wait "$signal_pid" 2>/dev/null
        [ "$?" -eq 1 ] || signal_failure=1
        health=$("$CHUSFS" -l usfs0 2>/dev/null) || signal_failure=1
        echo "$health" | grep -q '^active_connections=0$' || signal_failure=1
    fi
    [ "$signal_failure" -eq 0 ] || {
        sed 's/^/# /' "$BASE/signal.out"
        sed 's/^/# /' "$BASE/signal.excess.out"
    }
    stop_daemon
    tap_ok "$signal_failure" "repeated delivered-request signals preserve queue ownership and complete by $completion"
done

final_failure=0
restore_policy || final_failure=1
recovery_works || final_failure=1
no_usfs_state || final_failure=1
tap_ok "$final_failure" "availability journey restores defaults and leaves USFS quiescent"
tap_finish
