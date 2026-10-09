#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise the complete USFS wire protocol, including all read-only and
#   mutating opcodes plus malformed, delayed, duplicate, and reordered replies.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/protocol.sh
#
# Notes:
#   Scenario files program the test daemon one reply at a time. The test emits
#   TAP and requires bad channels to fail without preventing fresh recovery.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-contract.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
BARRIERS=$BASE/barriers

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

start_daemon()
{
    mkdir -p "$MOUNT"
    rm -rf "$BARRIERS"
    mkdir -p "$BARRIERS"
    USFS_SCENARIO_BARRIER_DIR="$BARRIERS" \
        /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "scenario mount" 30 is_mounted
}

stop_daemon()
{
    touch "$BARRIERS/after-read.release" 2>/dev/null
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    kill -TERM "$DAEMON_PID" 2>/dev/null
    usfs_wait_until "scenario daemon exit" 15 sh -c "! ps -p $DAEMON_PID >/dev/null 2>&1"
    wait "$DAEMON_PID" 2>/dev/null
    rmdir "$MOUNT" 2>/dev/null
}

has_opcode()
{
    awk -v opcode="$1" '$1 == opcode { found=1 } END { exit !found }' "$REQUESTS"
}

mutation()
{
    description=$1
    shift
    if "$@" >/dev/null 2>>"$DAEMON_LOG"; then
        return 0
    fi
    tap_diag "mutation failed: $description"
    return 1
}

rejected_reply_case()
{
    action=$1
    tap_diag "scenario action=$action"
    rm -f "$REQUESTS" "$DAEMON_LOG"
    printf 'USFS-SCENARIO 1\nLOOKUP %s 0 1\n' "$action" >"$SCENARIO"
    failure=0
    if start_daemon; then
        ls "$MOUNT/file" >/dev/null 2>&1 && failure=1
    else
        failure=1
    fi
    stop_daemon >/dev/null 2>&1
    tap_ok "$failure" "$2"
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$BASE"
tap_plan 22

printf 'invalid\n' >"$SCENARIO"
if /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
   >"$DAEMON_LOG" 2>&1; then
    tap_ok 1 "invalid scenario is rejected"
else
    tap_ok 0 "invalid scenario is rejected"
fi

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
if start_daemon; then
    tap_ok 0 "direct protocol daemon mounts"
else
    sed 's/^/# /' "$DAEMON_LOG"
    tap_ok 1 "direct protocol daemon mounts"
fi

content=$(cat "$MOUNT/file" 2>/dev/null)
tap_is "$content" "contract-data" "LOOKUP OPEN READ RELEASE round trip"

listing=$(ls "$MOUNT" 2>/dev/null)
case "$listing" in
    *file*dir*|*dir*file*) tap_ok 0 "READDIR returns deterministic namespace" ;;
    *) tap_diag "listing='$listing'"; tap_ok 1 "READDIR returns deterministic namespace" ;;
esac

ls -ld "$MOUNT/file" >/dev/null 2>&1
target=$(perl -e 'print readlink($ARGV[0])' "$MOUNT/link" 2>/dev/null)
tap_is "$target" "file" "READLINK returns exact target"

df "$MOUNT" >/dev/null 2>&1
tap_ok $? "STATFS request succeeds"

core_missing=0
for opcode in 1 2 3 4 5 6 7 8; do
    if ! has_opcode "$opcode"; then
        tap_diag "required opcode $opcode was not observed"
        core_missing=$((core_missing + 1))
    fi
done
tap_ok "$core_missing" "all eight read-only protocol opcodes observed"

if stop_daemon; then
    tap_ok 0 "successful scenario cleans up"
else
    tap_ok 1 "successful scenario cleans up"
fi

rm -f "$REQUESTS" "$DAEMON_LOG"
printf 'USFS-SCENARIO 1\nLOOKUP error 13 1\n' >"$SCENARIO"
if start_daemon; then
    if ls "$MOUNT/file" >/dev/null 2>&1; then
        tap_ok 1 "daemon errno is returned to caller"
    else
        tap_ok 0 "daemon errno is returned to caller"
    fi
else
    tap_ok 1 "daemon errno is returned to caller"
fi
stop_daemon >/dev/null 2>&1

rm -f "$REQUESTS" "$DAEMON_LOG"
printf 'USFS-SCENARIO 1\nLOOKUP wrong-version 0 1\n' >"$SCENARIO"
if start_daemon; then
    if ls "$MOUNT/file" >/dev/null 2>&1; then
        tap_ok 1 "wrong reply version is rejected"
    else
        tap_ok 0 "wrong reply version is rejected"
    fi
else
    tap_ok 1 "wrong reply version is rejected"
fi
stop_daemon >/dev/null 2>&1

rm -f "$REQUESTS" "$DAEMON_LOG"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
mutation_failures=0
if start_daemon; then
    mutation mkdir mkdir "$MOUNT/newdir" || mutation_failures=$((mutation_failures + 1))
    if ! printf 'abc' >"$MOUNT/newfile" 2>>"$DAEMON_LOG"; then
        tap_diag "mutation failed: create/write"
        mutation_failures=$((mutation_failures + 1))
    fi
    mutation chmod chmod 0600 "$MOUNT/file" || mutation_failures=$((mutation_failures + 1))
    mutation link ln "$MOUNT/file" "$MOUNT/hard" || mutation_failures=$((mutation_failures + 1))
    mutation symlink ln -s file "$MOUNT/soft" || mutation_failures=$((mutation_failures + 1))
    mutation rename mv "$MOUNT/hard" "$MOUNT/moved" || mutation_failures=$((mutation_failures + 1))
    mutation unlink rm "$MOUNT/moved" "$MOUNT/newfile" "$MOUNT/soft" || mutation_failures=$((mutation_failures + 1))
    mutation rmdir rmdir "$MOUNT/newdir" || mutation_failures=$((mutation_failures + 1))
else
    mutation_failures=$((mutation_failures + 1))
fi
tap_ok "$mutation_failures" "mutating operations return successful filesystem results"

mutation_missing=0
for opcode in 21 10 11 12 13 14 15 16 17; do
    if ! has_opcode "$opcode"; then
        tap_diag "required mutating opcode $opcode was not observed"
        mutation_missing=$((mutation_missing + 1))
    fi
done
tap_ok "$mutation_missing" "all nine mutating protocol opcodes observed"
stop_daemon >/dev/null 2>&1

rejected_reply_case wrong-opcode "wrong reply opcode is rejected"
rejected_reply_case wrong-unique "wrong reply unique id is rejected"
rejected_reply_case truncated "truncated reply header is rejected"
rejected_reply_case oversized "oversized reply length is rejected"
rejected_reply_case wrong-body "malformed operation body is rejected"

rm -f "$REQUESTS" "$DAEMON_LOG"
tap_diag "scenario action=disconnect"
printf 'USFS-SCENARIO 1\nLOOKUP disconnect 0 1\n' >"$SCENARIO"
disconnect_failure=0
if start_daemon; then
    ls "$MOUNT/file" >/dev/null 2>&1 && disconnect_failure=1
else
    disconnect_failure=1
fi
if ! usfs_wait_until "disconnect daemon exit" 15 \
   sh -c "! ps -p $DAEMON_PID >/dev/null 2>&1"; then
    disconnect_failure=1
fi
stop_daemon >/dev/null 2>&1
tap_ok "$disconnect_failure" "daemon disconnect wakes the filesystem caller"

rm -f "$REQUESTS" "$DAEMON_LOG"
tap_diag "scenario action=delay"
printf 'USFS-SCENARIO 1\nLOOKUP delay 250 1\n' >"$SCENARIO"
delay_failure=0
if start_daemon; then
    content=$(cat "$MOUNT/file" 2>/dev/null)
    [ "$content" = "contract-data" ] || delay_failure=1
else
    delay_failure=1
fi
stop_daemon >/dev/null 2>&1
tap_ok "$delay_failure" "delayed reply remains valid"

rm -f "$REQUESTS" "$DAEMON_LOG"
tap_diag "scenario action=duplicate"
printf 'USFS-SCENARIO 1\nLOOKUP duplicate 0 1\n' >"$SCENARIO"
duplicate_failure=0
if start_daemon; then
    cat "$MOUNT/file" >/dev/null 2>&1
    duplicate_first_rc=$?
    cat "$MOUNT/file" >/dev/null 2>&1
    duplicate_second_rc=$?
    grep -q '^DUPLICATE_REJECTED ' "$REQUESTS"
    duplicate_log_rc=$?
    [ "$duplicate_first_rc" -ne 0 ] || duplicate_failure=1
    [ "$duplicate_second_rc" -ne 0 ] || duplicate_failure=1
    [ "$duplicate_log_rc" -eq 0 ] || duplicate_failure=1
    tap_diag "duplicate first_rc=$duplicate_first_rc second_rc=$duplicate_second_rc log_rc=$duplicate_log_rc"
else
    duplicate_failure=1
fi
stop_daemon >/dev/null 2>&1
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
if start_daemon; then
    cat "$MOUNT/file" >/dev/null 2>&1
    duplicate_recovery_rc=$?
    [ "$duplicate_recovery_rc" -eq 0 ] || duplicate_failure=1
    tap_diag "duplicate recovery_rc=$duplicate_recovery_rc"
else
    duplicate_failure=1
fi
stop_daemon >/dev/null 2>&1
tap_ok "$duplicate_failure" "duplicate reply terminates the bad channel and a fresh channel recovers"

rm -f "$REQUESTS" "$DAEMON_LOG"
tap_diag "scenario action=reply-after-signal"
printf 'USFS-SCENARIO 1\nLOOKUP hold 30000 1\n' >"$SCENARIO"
cancel_failure=0
if start_daemon; then
    cat "$MOUNT/file" >/dev/null 2>&1 &
    canceled_pid=$!
    usfs_track_pid "$canceled_pid"
    usfs_wait_until "delivered request before signal" 10 \
        test -f "$BARRIERS/after-read.ready" ||
        cancel_failure=1
    kill -TERM "$canceled_pid" 2>/dev/null
    touch "$BARRIERS/after-read.release"
    wait "$canceled_pid" 2>/dev/null
    [ "$?" -ne 0 ] || cancel_failure=1
    cat "$MOUNT/file" >/dev/null 2>&1 || cancel_failure=1
else
    cancel_failure=1
fi
[ "$cancel_failure" -eq 0 ] || sed 's/^/# /' "$REQUESTS"
stop_daemon >/dev/null 2>&1
tap_ok "$cancel_failure" "delivered reply before signaled process teardown preserves channel"

rm -f "$REQUESTS" "$DAEMON_LOG"
tap_diag "scenario action=reordered"
printf 'USFS-SCENARIO 1\nLOOKUP reordered 0 1\n' >"$SCENARIO"
reorder_failure=0
if start_daemon; then
    cat "$MOUNT/file" >"$BASE/reordered.a" 2>/dev/null &
    first_pid=$!
    usfs_track_pid "$first_pid"
    usfs_wait_until "first reorder request" 10 \
        sh -c "[ -f '$REQUESTS' ] && [ \$(awk '\$1 == 1 { n++ } END { print n+0 }' '$REQUESTS') -ge 1 ]" ||
        reorder_failure=1
    cat "$MOUNT/file" >"$BASE/reordered.b" 2>/dev/null &
    second_pid=$!
    usfs_track_pid "$second_pid"
    wait "$first_pid" || reorder_failure=1
    wait "$second_pid" || reorder_failure=1
    [ "$(cat "$BASE/reordered.a" 2>/dev/null)" = "contract-data" ] ||
        reorder_failure=1
    [ "$(cat "$BASE/reordered.b" 2>/dev/null)" = "contract-data" ] ||
        reorder_failure=1
else
    reorder_failure=1
fi
stop_daemon >/dev/null 2>&1
tap_ok "$reorder_failure" "out-of-order completion preserves unique-id correlation"

tap_finish
