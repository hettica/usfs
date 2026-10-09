#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise malformed OPEN/CREATE/RELEASE replies and post-reply disconnects,
#   then prove channel recovery and balanced vnode/handle ownership.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/handle_lifecycle.sh
#
# Notes:
#   Requires USFS_TESTING accounting controls. The final assertion checks live,
#   created, reclaimed, hold, release, duplicate, and accounting-error counts.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-handle-lifecycle.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
BARRIERS=$BASE/barriers
DAEMON_PID=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

start_daemon()
{
    rm -f "$REQUESTS" "$DAEMON_LOG"
    rm -rf "$BARRIERS"
    mkdir -p "$BARRIERS"
    USFS_SCENARIO_BARRIER_DIR="$BARRIERS" \
        /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "handle-lifecycle mount" 30 is_mounted
}

stop_daemon()
{
    touch "$BARRIERS/after-read.release" 2>/dev/null
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

action_observed()
{
    grep -q "^ACTION $1 " "$REQUESTS" 2>/dev/null
}

status_value()
{
    printf '%s\n' "$1" |
        awk -v key="$2" '{
            for (i = 1; i <= NF; i++) {
                split($i, pair, "=")
                if (pair[1] == key) {
                    print pair[2]
                    exit
                }
            }
        }'
}

fresh_recovery()
{
    stop_daemon
    printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
    start_daemon || return 1
    [ "$(cat "$MOUNT/file" 2>/dev/null)" = "contract-data" ]
}

quarantined_ownership()
{
    expected_handles=$1
    expected_errno=${2:-22}
    usfs_wait_until "malformed ownership daemon exit" 15 \
        sh -c "! ps -p $DAEMON_PID >/dev/null 2>&1" || return 1
    ownership=$(grep '^OWNERSHIP ' "$REQUESTS")
    [ "$(status_value "$ownership" created)" = "$expected_handles" ] &&
        [ "$(status_value "$ownership" released)" = 0 ] &&
        [ "$(status_value "$ownership" closed)" = "$expected_handles" ] &&
        [ "$(status_value "$ownership" lookup_closed)" -gt 0 ] &&
        [ "$(status_value "$ownership" errors)" = 0 ] &&
        grep -q "^REPLY_REJECTED errno=$expected_errno\$" "$REQUESTS"
}

open_validation_case()
{
    action=$1
    case_label=$2
    failure=0
    printf 'USFS-SCENARIO 1\nOPEN %s 0 1\n' "$action" >"$SCENARIO"
    start_daemon || failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
    action_observed 3 || failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
    copy_errno=22
    [ "$action" != reply-copy-fault ] || copy_errno=14
    quarantined_ownership 1 "$copy_errno" || failure=1
    fresh_recovery || failure=1
    stop_daemon
    tap_ok "$failure" "$case_label quarantines ownership and a fresh channel recovers"
}

create_validation_case()
{
    action=$1
    case_label=$2
    failure=0
    printf 'USFS-SCENARIO 1\nCREATE_ATTR %s 0 1\n' "$action" >"$SCENARIO"
    start_daemon || failure=1
    touch "$MOUNT/newfile" >/dev/null 2>&1 && failure=1
    action_observed 21 || failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
    copy_errno=22
    [ "$action" != reply-copy-fault ] || copy_errno=14
    quarantined_ownership 1 "$copy_errno" || failure=1
    fresh_recovery || failure=1
    stop_daemon
    tap_ok "$failure" "$case_label quarantines ownership and a fresh channel recovers"
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
tap_plan 28

/usr/sbin/usfs_io_probe readiness /dev/usfs0 >"$BASE/readiness.log" 2>&1
tap_ok "$?" "native poll accepts AIX hangup flags on an idle transport"

if /usr/sbin/usfs_testctl reset >/dev/null 2>&1; then
    tap_ok 0 "handle lifecycle accounting resets while quiescent"
else
    tap_ok 1 "handle lifecycle accounting resets while quiescent"
fi

open_validation_case short-body "short OPEN body"
open_validation_case invalid-open-pad "nonzero OPEN padding"
create_validation_case short-body "short CREATE body"
create_validation_case invalid-attr "invalid CREATE attributes"
open_validation_case reply-copy-fault "failed OPEN reply copy"
create_validation_case reply-copy-fault "failed CREATE reply copy"

for action in short-body invalid-nodeid reply-copy-fault; do
    failure=0
    printf 'USFS-SCENARIO 1\nLOOKUP %s 0 1\n' "$action" >"$SCENARIO"
    start_daemon || failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
    copy_errno=22
    [ "$action" != reply-copy-fault ] || copy_errno=14
    quarantined_ownership 0 "$copy_errno" || failure=1
    fresh_recovery || failure=1
    stop_daemon
    tap_ok "$failure" "LOOKUP $action quarantines the allocated reference and a fresh channel recovers"
done

failure=0
printf 'USFS-SCENARIO 1\nOPEN reply-disconnect 0 1\n' >"$SCENARIO"
start_daemon || failure=1
cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
action_observed 3 || failure=1
usfs_wait_until "post-open disconnect" 15 \
    sh -c "! ps -p $DAEMON_PID >/dev/null 2>&1" || failure=1
fresh_recovery || failure=1
stop_daemon
tap_ok "$failure" "disconnect after a successful OPEN releases kernel ownership and a fresh channel recovers"

failure=0
printf 'USFS-SCENARIO 1\nRELEASE error 5 1\n' >"$SCENARIO"
start_daemon || failure=1
[ "$(cat "$MOUNT/file" 2>/dev/null)" = "contract-data" ] || failure=1
action_observed 5 || failure=1
cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
fresh_recovery || failure=1
stop_daemon
tap_ok "$failure" "failed RELEASE terminates uncertain handle ownership and a fresh channel recovers"

failure=0
printf 'USFS-SCENARIO 1\nRELEASE wrong-body 0 1\n' >"$SCENARIO"
start_daemon || failure=1
[ "$(cat "$MOUNT/file" 2>/dev/null)" = "contract-data" ] || failure=1
action_observed 5 || failure=1
cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
fresh_recovery || failure=1
stop_daemon
tap_ok "$failure" "RELEASE body overflow terminates the bad channel and a fresh channel recovers"

failure=0
printf 'USFS-SCENARIO 1\nRELEASE reply-disconnect 0 1\n' >"$SCENARIO"
start_daemon || failure=1
[ "$(cat "$MOUNT/file" 2>/dev/null)" = "contract-data" ] || failure=1
action_observed 5 || failure=1
usfs_wait_until "post-release disconnect" 15 \
    sh -c "! ps -p $DAEMON_PID >/dev/null 2>&1" || failure=1
fresh_recovery || failure=1
stop_daemon
tap_ok "$failure" "disconnect after a successful RELEASE leaves no stranded handle"

failure=0
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
start_daemon || failure=1
dual_result=$(/usr/sbin/usfs_io_probe dual-open "$MOUNT/file" 2>/dev/null)
echo "$dual_result" | grep -q 'result=0' || failure=1
tap_ok "$failure" \
    "simultaneous read-only and write-only descriptions remain usable"

open_first=$(awk '$1 == 3 { print $2; exit }' "$REQUESTS" 2>/dev/null)
open_second=$(awk '$1 == 3 { count++; if (count == 2) { print $2; exit } }' \
    "$REQUESTS" 2>/dev/null)
open_count=$(grep -c '^3 ' "$REQUESTS" 2>/dev/null)
if [ "$open_count" -eq 2 ] && [ -n "$open_first" ] &&
   [ -n "$open_second" ] && [ "$open_first" != "$open_second" ] &&
   grep -q "^HANDLE 4 $open_first\$" "$REQUESTS" &&
   [ "$(grep -c "^HANDLE 17 $open_second\$" "$REQUESTS")" -eq 2 ]; then
    tap_ok 0 "each open receives and routes its distinct daemon handle"
else
    tap_diag "open_first=$open_first open_second=$open_second open_count=$open_count"
    tap_diag "handle trace: $(grep '^HANDLE ' "$REQUESTS" | tr '\n' ';')"
    tap_ok 1 "each open receives and routes its distinct daemon handle"
fi

if [ "$(grep -c "^HANDLE 5 $open_first\$" "$REQUESTS")" -eq 1 ] &&
   [ "$(grep -c "^HANDLE 5 $open_second\$" "$REQUESTS")" -eq 1 ]; then
    tap_ok 0 "each open description releases exactly its own handle"
else
    tap_diag "release trace: $(grep '^HANDLE 5 ' "$REQUESTS" | tr '\n' ';')"
    tap_ok 1 "each open description releases exactly its own handle"
fi
stop_daemon

failure=0
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
start_daemon || failure=1
/usr/sbin/usfs_io_probe retained-handles "$MOUNT/file" >"$BASE/retained.log" 2>&1 || failure=1
[ "$failure" -eq 0 ] || sed 's/^/# /' "$BASE/retained.log"
tap_ok "$failure" "getattr, fsync, and first page-in remain valid after descriptor reuse"
old_handle=$(awk '$1 == 3 { print $2; exit }' "$REQUESTS")
new_handle=$(awk '$1 == 3 { n++; if (n == 2) { print $2; exit } }' "$REQUESTS")
failure=0
[ -n "$old_handle" ] && [ -n "$new_handle" ] || failure=1
grep -q "^HANDLE 4 $old_handle\$" "$REQUESTS" || failure=1
[ "$(grep -c "^HANDLE 5 $old_handle\$" "$REQUESTS")" -eq 1 ] || failure=1
[ "$(grep -c "^HANDLE 5 $new_handle\$" "$REQUESTS")" -eq 1 ] || failure=1
[ "$(awk '$1 == "HANDLE" && $2 == 5 { print $3; exit }' "$REQUESTS")" = "$new_handle" ] || failure=1
[ "$failure" -eq 0 ] || sed 's/^/# /' "$REQUESTS"
tap_ok "$failure" "replacement releases before final unmap releases the original handle exactly once"
stop_daemon

for operation in open create; do
    failure=0
    case "$operation" in
        open) opcode=3; wire_operation=OPEN; filename=file ;;
        create) opcode=21; wire_operation=CREATE_ATTR; filename=signaled-new ;;
    esac
    printf 'USFS-SCENARIO 1\n%s hold 30000 1\n' \
        "$wire_operation" >"$SCENARIO"
    start_daemon || failure=1
    /usr/sbin/usfs_io_probe "signaled-$operation" "$MOUNT/$filename" \
        >"$BASE/signaled.out" 2>&1 &
    probe_pid=$!
    usfs_track_pid "$probe_pid"
    usfs_wait_until "delivered $operation before signal" 10 \
        test -f "$BARRIERS/after-read.ready" || failure=1
    kill -USR1 "$probe_pid" || failure=1
    kill -USR1 "$probe_pid" || failure=1
    touch "$BARRIERS/after-read.release"
    wait "$probe_pid" 2>/dev/null
    [ "$?" -eq 143 ] || failure=1
    grep -q '^result=0 errno=0 signal=1$' "$BASE/signaled.out" || failure=1
    handle=$(awk -v op="$opcode" '$1 == op { print $2; exit }' "$REQUESTS")
    [ "$opcode" -ne 21 ] || handle=$((handle + 100))
    [ -n "$handle" ] &&
        [ "$(grep -c "^HANDLE 5 $handle\$" "$REQUESTS")" -eq 1 ] || failure=1
    /usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/after-signal.out" 2>&1
    grep -q '^result=0 errno=0$' "$BASE/after-signal.out" || failure=1
    [ "$failure" -eq 0 ] || {
        sed 's/^/# /' "$BASE/signaled.out"
        sed 's/^/# /' "$REQUESTS"
    }
    stop_daemon
    tap_ok "$failure" "signaled $operation completes and process termination releases its handle exactly once"
done

failure=0
USFS_MT_CREATE_FLAGS=1 /usr/sbin/usfs_mt_test_daemon -s "$MOUNT" >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"
usfs_wait_until "client CREATE access modes" 30 is_mounted || failure=1
if [ "$failure" -eq 0 ]; then
    /usr/sbin/usfs_io_probe create-flags "$MOUNT" >"$BASE/create-flags.log" 2>&1 || failure=1
fi
tap_ok "$failure" "created read-only write-only and read-write descriptions enforce their access modes"
for mode in 0 1 2; do
    handle=$((mode + 1))
    [ "$(grep -c "^CREATE $mode flags=$mode fh=$handle$" "$DAEMON_LOG")" -eq 1 ] || failure=1
    [ "$(grep -c "^RELEASE $mode flags=$mode fh=$handle valid=1$" "$DAEMON_LOG")" -eq 1 ] || failure=1
done
[ "$(grep -c '^CREATE ' "$DAEMON_LOG")" -eq 3 ] || failure=1
[ "$(grep -c '^RELEASE ' "$DAEMON_LOG")" -eq 3 ] || failure=1
[ "$failure" -eq 0 ] || cat "$BASE/create-flags.log" "$DAEMON_LOG"
stop_daemon
tap_ok "$failure" "client callbacks receive matching CREATE and exactly-once RELEASE flags without unsupported mutation"

for action in error disconnect; do
    failure=0
    if [ "$action" = error ]; then value=5; else value=0; fi
    printf 'USFS-SCENARIO 1\nFORGET %s %s 1\n' "$action" "$value" >"$SCENARIO"
    start_daemon || failure=1
    /usr/sbin/usfs_io_probe stat "$MOUNT/file" >"$BASE/forget.out" 2>&1
    action_observed 22 || failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 && failure=1
    [ "$failure" -eq 0 ] || sed 's/^/# /' "$REQUESTS"
    fresh_recovery || failure=1
    stop_daemon
    tap_ok "$failure" "FORGET $action terminates uncertain ownership and a fresh channel recovers"
done

failure=0
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
start_daemon || failure=1
[ "$(cat "$MOUNT/file" 2>/dev/null)" = contract-data ] || failure=1
umount "$MOUNT" >/dev/null 2>&1 || failure=1
usfs_wait_until "raw daemon orderly EOF" 15 \
    sh -c "! ps -p $DAEMON_PID >/dev/null 2>&1" || failure=1
if [ "$failure" -eq 0 ]; then
    wait "$DAEMON_PID" || failure=1
    DAEMON_PID=0
fi
grep -q '^ORDERLY_EOF$' "$REQUESTS" || failure=1
grep -q '^OWNERSHIP created=1 released=1 closed=0 lookup_closed=0 errors=0$' "$REQUESTS" || failure=1
stop_daemon
tap_ok "$failure" "ordinary external unmount returns EOF after balanced raw-protocol cleanup without TERM"

node_status=$(/usr/sbin/usfs_testctl node-status 2>/dev/null)
created=$(status_value "$node_status" created)
reclaimed=$(status_value "$node_status" reclaimed)
live=$(status_value "$node_status" live)
holds=$(status_value "$node_status" holds)
releases=$(status_value "$node_status" releases)
duplicate_live=$(status_value "$node_status" duplicate_live)
accounting_errors=$(status_value "$node_status" accounting_errors)
if [ "$live" -eq 0 ] &&
   [ "$created" -eq "$reclaimed" ] &&
   [ $((created + holds)) -eq "$releases" ] &&
   [ "$duplicate_live" -eq 0 ] &&
   [ "$accounting_errors" -eq 0 ]; then
    tap_ok 0 "all handle edge cases finish with balanced vnode ownership"
else
    tap_diag "node status: $node_status"
    tap_ok 1 "all handle edge cases finish with balanced vnode ownership"
fi

tap_finish
