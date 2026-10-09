#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise the AIX client pager with hostile daemon replies, disconnects,
#   cancellation, large sparse objects, concurrent faults, and allocation
#   failures. Every wait is bounded so a pager regression is reported instead
#   of silently wedging the VM test run.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/mmap_pager.sh

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-mmap-pager.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
BARRIERS=$BASE/barriers
DAEMON_PID=0
PROBE_PID=0
PROBE_OUT=
PROBE_RC=
BACKGROUND_SYNC_TIMEOUT_SECONDS=10

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

daemon_exited()
{
    ! ps -p "$DAEMON_PID" >/dev/null 2>&1
}

probe_done()
{
    test -f "$BASE/probe.rc"
}

probe_exited()
{
    ! ps -p "$PROBE_PID" >/dev/null 2>&1
}

background_sync_recorded()
{
    awk '$1 == 20 && $7 == 0 { found=1 } END { exit !found }' "$REQUESTS"
}

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
    usfs_wait_until "mmap pager mount" 30 is_mounted
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

spawn_probe()
{
    rm -f "$BASE/probe.out" "$BASE/probe.err" "$BASE/probe.rc"
    (
        "$@" >"$BASE/probe.out" 2>"$BASE/probe.err"
        printf '%s\n' "$?" >"$BASE/probe.rc"
    ) &
    PROBE_PID=$!
    usfs_track_pid "$PROBE_PID"
}

spawn_probe_direct()
{
    rm -f "$BASE/probe.out" "$BASE/probe.err" "$BASE/probe.rc"
    "$@" >"$BASE/probe.out" 2>"$BASE/probe.err" &
    PROBE_PID=$!
    usfs_track_pid "$PROBE_PID"
}

wait_probe()
{
    if ! usfs_wait_until "mmap probe" 20 probe_done; then
        kill -KILL "$PROBE_PID" 2>/dev/null
        wait "$PROBE_PID" 2>/dev/null
        PROBE_OUT=timeout
        return 1
    fi
    wait "$PROBE_PID" 2>/dev/null
    PROBE_OUT=$(cat "$BASE/probe.out" 2>/dev/null)
    PROBE_RC=$(cat "$BASE/probe.rc" 2>/dev/null)
    return 0
}

probe_faulted()
{
    echo "$PROBE_OUT" | grep -q 'result=-1' &&
        echo "$PROBE_OUT" | grep -q 'signal=[1-9]'
}

recovery_works()
{
    printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
    start_daemon 8192 || return 1
    spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read \
        "$MOUNT/file" 8192 0 4097
    wait_probe || {
        stop_daemon
        return 1
    }
    ok=1
    echo "$PROBE_OUT" | grep -q 'result=0 errno=0 signal=0 value=1' || ok=0
    stop_daemon
    [ "$ok" -eq 1 ]
}

no_usfs_state()
{
    mounts=$(mount 2>/dev/null | awk '$3 == "usfs" { n++ } END { print n+0 }')
    daemons=$(ps -ef | grep -c '[u]sfs_scenario_daemon')
    [ "$mounts" -eq 0 ] && [ "$daemons" -eq 0 ]
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT" "$BARRIERS"
tap_plan 30

read_matrix_failure=0
for action in wrong-version wrong-opcode wrong-unique truncated oversized \
              invalid-read-count; do
    printf 'USFS-SCENARIO 1\nREAD %s\n' "$action" >"$SCENARIO"
    start_daemon 8192 || read_matrix_failure=1
    spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read \
        "$MOUNT/file" 4096 0 17
    wait_probe || read_matrix_failure=1
    probe_faulted || {
        tap_diag "READ $action: $PROBE_OUT"
        read_matrix_failure=1
    }
    stop_daemon
done
recovery_works || read_matrix_failure=1
tap_ok "$read_matrix_failure" "malformed page-in replies fault locally and fresh channels recover"

printf 'USFS-SCENARIO 1\nREAD error 5\n' >"$SCENARIO"
read_error_failure=0
start_daemon 8192 || read_error_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read "$MOUNT/file" 4096 0 0
wait_probe || read_error_failure=1
probe_faulted || read_error_failure=1
stop_daemon
recovery_works || read_error_failure=1
tap_ok "$read_error_failure" "daemon page-in errors become process-local mapping faults"

write_matrix_failure=0
for action in wrong-version wrong-unique truncated oversized wrong-body \
              short-body invalid-write-count invalid-write-offset short-write; do
    printf 'USFS-SCENARIO 1\nWRITE %s\n' "$action" >"$SCENARIO"
    start_daemon 8192 || write_matrix_failure=1
    spawn_probe /usr/sbin/usfs_io_probe mmap-write-shared \
        "$MOUNT/file" 4096 0
    wait_probe || write_matrix_failure=1
    echo "$PROBE_OUT" | grep -q 'result=-1' || {
        tap_diag "WRITE $action: $PROBE_OUT"
        write_matrix_failure=1
    }
    stop_daemon
done
printf 'USFS-SCENARIO 1\nWRITE error 5\n' >"$SCENARIO"
start_daemon 8192 || write_matrix_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-write-shared \
    "$MOUNT/file" 4096 0
wait_probe || write_matrix_failure=1
echo "$PROBE_OUT" | grep -q 'result=-1' || write_matrix_failure=1
stop_daemon
recovery_works || write_matrix_failure=1
tap_ok "$write_matrix_failure" "malformed dirty-page replies cannot report false success"

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
high_failure=0
start_daemon 4294975488 || high_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read \
    "$MOUNT/file" 4096 4294967296 123
wait_probe || high_failure=1
echo "$PROBE_OUT" | grep -q 'result=0 errno=0 signal=0 value=123' || \
    high_failure=1
grep -q '^DETAIL 4 4294967296 4096$' "$REQUESTS" 2>/dev/null || \
    high_failure=1
[ "$high_failure" -eq 0 ] || {
    tap_diag "high offset probe: $PROBE_OUT"
    tap_diag "high offset requests: $(grep '^DETAIL 4 ' "$REQUESTS" 2>/dev/null)"
}
stop_daemon
tap_ok "$high_failure" "pager preserves page-aligned offsets above 4 GiB"

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
growth_failure=0
start_daemon 8192 || growth_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-grow-large "$MOUNT/file"
wait_probe || growth_failure=1
[ "$PROBE_RC" = 0 ] || growth_failure=1
echo "$PROBE_OUT" | grep -q 'mmap-grow-large=valid' || growth_failure=1
[ "$growth_failure" -eq 0 ] || tap_diag "cache growth: $PROBE_OUT $(cat "$BASE/probe.err" 2>/dev/null)"
stop_daemon
tap_ok "$growth_failure" "existing small client segment grows past 4 GiB with coherent ordinary I/O"

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
huge_failure=0
start_daemon 1073741824 || huge_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read \
    "$MOUNT/file" 1073741824 0 1073741700
wait_probe || huge_failure=1
echo "$PROBE_OUT" | grep -q 'result=0 errno=0 signal=0 value=132' || \
    huge_failure=1
stop_daemon
tap_ok "$huge_failure" "one-gigabyte sparse mapping faults only the addressed page"

printf 'USFS-SCENARIO 1\nREAD reordered\n' >"$SCENARIO"
concurrent_failure=0
start_daemon 65536 || concurrent_failure=1
probe_pids=""
index=0
while [ "$index" -lt 8 ]; do
    (
        /usr/sbin/usfs_io_probe mmap-touch-read "$MOUNT/file" \
            4096 "$((index * 4096))" 7 >"$BASE/concurrent.$index"
        printf '%s\n' "$?" >"$BASE/concurrent.$index.rc"
    ) &
    probe_pids="$probe_pids $!"
    usfs_track_pid "$!"
    index=$((index + 1))
done
for probe_pid in $probe_pids; do
    wait "$probe_pid" 2>/dev/null || concurrent_failure=1
done
index=0
while [ "$index" -lt 8 ]; do
    grep -q 'result=0 errno=0 signal=0 value=7' \
        "$BASE/concurrent.$index" || concurrent_failure=1
    index=$((index + 1))
done
stop_daemon
tap_ok "$concurrent_failure" "eight processes fault distinct pages concurrently"

printf 'USFS-SCENARIO 1\nREAD hold 30000 1\n' >"$SCENARIO"
disconnect_read_failure=0
start_daemon 8192 || disconnect_read_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read "$MOUNT/file" 4096 0 0
usfs_wait_until "held page-in" 10 test -f "$BARRIERS/after-read.ready" || \
    disconnect_read_failure=1
kill_daemon || disconnect_read_failure=1
wait_probe || disconnect_read_failure=1
probe_faulted || disconnect_read_failure=1
stop_daemon
recovery_works || disconnect_read_failure=1
tap_ok "$disconnect_read_failure" "daemon death releases a delivered page-in waiter"

printf 'USFS-SCENARIO 1\nWRITE hold 30000 1\n' >"$SCENARIO"
disconnect_write_failure=0
start_daemon 8192 || disconnect_write_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-write-shared "$MOUNT/file" 4096 0
usfs_wait_until "held page-out" 10 test -f "$BARRIERS/after-read.ready" || \
    disconnect_write_failure=1
kill_daemon || disconnect_write_failure=1
wait_probe || disconnect_write_failure=1
echo "$PROBE_OUT" | grep -q 'result=-1' || disconnect_write_failure=1
stop_daemon
recovery_works || disconnect_write_failure=1
tap_ok "$disconnect_write_failure" "daemon death releases a dirty-page writeback waiter"

printf 'USFS-SCENARIO 1\nREAD hold 30000 1\n' >"$SCENARIO"
cancel_failure=0
start_daemon 8192 || cancel_failure=1
spawn_probe_direct /usr/sbin/usfs_io_probe mmap-touch-read \
    "$MOUNT/file" 4096 0 0
usfs_wait_until "cancelled page-in" 10 test -f "$BARRIERS/after-read.ready" || \
    cancel_failure=1
kill -TERM "$PROBE_PID" 2>/dev/null
touch "$BARRIERS/after-read.release"
usfs_wait_until "signaled mmap probe exit" 10 probe_exited || \
    cancel_failure=1
wait "$PROBE_PID" 2>/dev/null
spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read "$MOUNT/file" 4096 0 9
wait_probe || cancel_failure=1
echo "$PROBE_OUT" | grep -q 'result=0 errno=0 signal=0 value=9' || \
    cancel_failure=1
[ "$cancel_failure" -eq 0 ] || {
    tap_diag "cancel recovery probe: $PROBE_OUT"
    tap_diag "cancel requests: $(grep -E '^(ACTION|REPLIED|DETAIL 4)' "$REQUESTS" 2>/dev/null)"
}
stop_daemon
tap_ok "$cancel_failure" "signal during a fault completes without poisoning the channel"

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
idle_disconnect_failure=0
start_daemon 8192 || idle_disconnect_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-hold "$MOUNT/file" 8192 4096 \
    "$BASE/idle.ready" "$BASE/idle.release"
usfs_wait_until "idle mapped process" 10 test -f "$BASE/idle.ready" || \
    idle_disconnect_failure=1
kill_daemon || idle_disconnect_failure=1
touch "$BASE/idle.release"
wait_probe || idle_disconnect_failure=1
probe_faulted || idle_disconnect_failure=1
stop_daemon
recovery_works || idle_disconnect_failure=1
tap_ok "$idle_disconnect_failure" "idle mapping faults safely after its daemon disappears"

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
killed_process_failure=0
start_daemon 8192 || killed_process_failure=1
spawn_probe_direct /usr/sbin/usfs_io_probe mmap-hold \
    "$MOUNT/file" 8192 4096 \
    "$BASE/killed.ready" "$BASE/killed.release"
usfs_wait_until "mapped process before kill" 10 test -f "$BASE/killed.ready" || \
    killed_process_failure=1
kill -KILL "$PROBE_PID" 2>/dev/null
wait "$PROBE_PID" 2>/dev/null
stop_daemon
recovery_works || killed_process_failure=1
tap_ok "$killed_process_failure" "process death releases an active mapping and permits unmount"

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
allocation_failure=0
/usr/sbin/usfs_testctl arm 8 4 12 >/dev/null 2>&1 || allocation_failure=1
start_daemon 8192 || allocation_failure=1
spawn_probe /usr/sbin/usfs_io_probe mmap-touch-read \
    "$MOUNT/file" 4096 0 19
wait_probe || allocation_failure=1
if ! probe_faulted; then
    case "$PROBE_RC" in
        ""|0|*[!0-9]*) allocation_failure=1 ;;
    esac
fi
stop_daemon
/usr/sbin/usfs_testctl status >"$BASE/allocation.status" 2>&1 || \
    allocation_failure=1
grep -q 'fired=1' "$BASE/allocation.status" 2>/dev/null || \
    allocation_failure=1
[ "$allocation_failure" -eq 0 ] || {
    tap_diag "allocation probe: rc=$PROBE_RC out=$PROBE_OUT"
    tap_diag "allocation status: $(cat "$BASE/allocation.status" 2>/dev/null)"
    tap_diag "allocation requests: $(grep -E '^[0-9]|^DETAIL' "$REQUESTS" 2>/dev/null)"
}
/usr/sbin/usfs_testctl reset >/dev/null 2>&1 || allocation_failure=1
recovery_works || allocation_failure=1
tap_ok "$allocation_failure" "pager request-allocation failure completes the fault without leaks"

for kind in file range filesystem range-zero range-zero-eof range-zero-nocache range-zero-eof-nocache; do
    sync_failure=0
    printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
    start_daemon 8192 || sync_failure=1
    # An ordinary background sync may precede this probe's dirty pages. Keep
    # that request in the log so the ordering check must identify its caller.
    /usr/sbin/sync >"$BASE/pre-probe-sync.log" 2>&1 || sync_failure=1
    usfs_wait_until "background sync before $kind probe" "$BACKGROUND_SYNC_TIMEOUT_SECONDS" background_sync_recorded || sync_failure=1
    spawn_probe /usr/sbin/usfs_io_probe mmap-sync "$MOUNT/file" "$kind"
    wait_probe || sync_failure=1
    echo "$PROBE_OUT" | grep -q 'first=0 first_errno=0 second=0 second_errno=0' || sync_failure=1
    sync_probe_pid=$(printf '%s\n' "$PROBE_OUT" | sed -n 's/.* pid=\([0-9][0-9]*\).*/\1/p')
    case "$sync_probe_pid" in ""|0|*[!0-9]*) sync_failure=1 ;; esac
    if [ "$kind" = filesystem ]; then
        echo "$PROBE_OUT" | grep -q 'descriptors_closed=1' || sync_failure=1
    fi
    # Dirty-page WRITE must arrive before the durability barrier. Pager I/O
    # carries no originating process; the explicit barrier carries the probe.
    # A background SYNCFS before any dirtying is not that durability barrier.
    awk -v probe="$sync_probe_pid" '$1 == 17 { write_seen=1; if ($7 != 0) bad=1 }
         ($1 == 19 || $1 == 20) && $7 == probe { if (!write_seen) bad=1; barrier=1 }
         END { exit (!write_seen || !barrier || bad) }' "$REQUESTS" || sync_failure=1
    case "$kind" in
        range-zero*)
            # A later full fsync or unmap must not hide omitted pages in the
            # zero-length range: both dirty pages must precede its first barrier.
            awk -v probe="$sync_probe_pid" '$1 == "DETAIL" && $2 == 17 && !barrier {
                     if ($3 == 0 && $4 >= 4096) first_page=1
                     if ($3 <= 4096 && $3 + $4 >= 8192) second_page=1
                 }
                 $1 == 19 && $7 == probe && !barrier {
                     if (!first_page || !second_page) bad=1
                     barrier=1
                 }
                 END { exit (!barrier || bad) }' "$REQUESTS" || sync_failure=1
            ;;
    esac
    [ "$sync_failure" -eq 0 ] || {
        tap_diag "$kind sync: rc=$PROBE_RC out=$PROBE_OUT"
        tap_diag "$kind requests: $(cat "$REQUESTS" 2>/dev/null)"
        tap_diag "$kind probe errors: $(cat "$BASE/probe.err" 2>/dev/null)"
    }
    stop_daemon
    tap_ok "$sync_failure" "$kind sync submits mapped dirt before its barrier without preceding msync"

    sync_failure=0
    printf 'USFS-SCENARIO 1\nWRITE error 5\n' >"$SCENARIO"
    start_daemon 8192 || sync_failure=1
    /usr/sbin/sync >"$BASE/pre-probe-sync.log" 2>&1 || sync_failure=1
    usfs_wait_until "background sync before $kind error probe" "$BACKGROUND_SYNC_TIMEOUT_SECONDS" background_sync_recorded || sync_failure=1
    spawn_probe /usr/sbin/usfs_io_probe mmap-sync "$MOUNT/file" "$kind"
    wait_probe || sync_failure=1
    echo "$PROBE_OUT" | grep -q 'first=-1 first_errno=5 second=-1 second_errno=5' || sync_failure=1
    sync_probe_pid=$(printf '%s\n' "$PROBE_OUT" | sed -n 's/.* pid=\([0-9][0-9]*\).*/\1/p')
    case "$sync_probe_pid" in ""|0|*[!0-9]*) sync_failure=1 ;; esac
    awk -v probe="$sync_probe_pid" '($1 == 19 || $1 == 20) && $7 == probe { found=1 }
        END { exit !found }' "$REQUESTS" && sync_failure=1
    [ "$sync_failure" -eq 0 ] || tap_diag "$kind writeback: $PROBE_OUT"
    stop_daemon
    recovery_works || sync_failure=1
    tap_ok "$sync_failure" "$kind sync retains a pageout failure across a later successful writeback"
done

for kind in mapped cached; do
    eviction_failure=0
    /usr/sbin/usfs_testctl schedule 4 >/dev/null 2>&1 || eviction_failure=1
    /usr/sbin/usfs_memfs -f "$MOUNT" >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "eviction memfs mount" 30 is_mounted || eviction_failure=1
    spawn_probe /usr/sbin/usfs_io_probe cache-eviction "$MOUNT/eviction" "$kind"
    wait_probe || eviction_failure=1
    [ "$PROBE_RC" = 0 ] || eviction_failure=1
    echo "$PROBE_OUT" | grep -q 'cache-eviction=valid' || eviction_failure=1
    stop_daemon
    /usr/sbin/usfs_testctl reset >/dev/null 2>&1 || eviction_failure=1
    [ "$eviction_failure" -eq 0 ] || tap_diag "$kind eviction: $PROBE_OUT"
    tap_ok "$eviction_failure" "FNOCACHE preserves a $kind write between writeback and eviction through read-only range sync"
done

final_failure=0
no_usfs_state || final_failure=1
/usr/sbin/usfs_testctl info >/dev/null 2>&1 || final_failure=1
tap_ok "$final_failure" "pager matrix leaves a usable quiescent extension"

tap_finish
