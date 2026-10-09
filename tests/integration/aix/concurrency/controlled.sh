#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise deterministic concurrent lookup/read behavior and prove that
#   daemon closure wakes an in-flight filesystem caller without residue.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/concurrency/controlled.sh
#
# Notes:
#   Requires the USFS_TESTING extension and scenario daemon. The test emits
#   TAP, bounds every wait, and removes only mounts/processes it owns.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-concurrency.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

request_seen()
{
    [ -f "$REQUESTS" ] && awk '$1 == 1 { found=1 } END { exit !found }' "$REQUESTS"
}

start_daemon()
{
    rm -f "$REQUESTS" "$DAEMON_LOG"
    /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "concurrency mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

no_usfs_state()
{
    mounts=$(mount 2>/dev/null | awk '$3 == "usfs" { n++ } END { print n+0 }')
    daemons=$(ps -ef | grep -c '[u]sfs_scenario_daemon')
    [ "$mounts" -eq 0 ] && [ "$daemons" -eq 0 ]
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
tap_plan 7

printf 'USFS-SCENARIO 1\nLOOKUP delay 750 1\n' >"$SCENARIO"
if start_daemon; then
    tap_ok 0 "controlled-delay daemon mounts"
else
    tap_ok 1 "controlled-delay daemon mounts"
fi

cat "$MOUNT/file" >"$BASE/a" 2>"$BASE/a.err" &
A_PID=$!
usfs_track_pid "$A_PID"
if usfs_wait_until "first delayed lookup barrier" 10 request_seen; then
    tap_ok 0 "first lookup reached deterministic delay barrier"
else
    tap_ok 1 "first lookup reached deterministic delay barrier"
fi
cat "$MOUNT/file" >"$BASE/b" 2>"$BASE/b.err" &
B_PID=$!
usfs_track_pid "$B_PID"
wait "$A_PID"; A_RC=$?
wait "$B_PID"; B_RC=$?
if [ "$A_RC" -eq 0 ] && [ "$B_RC" -eq 0 ] &&
   [ "$(cat "$BASE/a")" = "contract-data" ] &&
   [ "$(cat "$BASE/b")" = "contract-data" ]; then
    tap_ok 0 "queued concurrent lookups both complete correctly"
else
    tap_ok 1 "queued concurrent lookups both complete correctly"
fi

reader_failures=0
reader_pids=""
i=1
while [ "$i" -le 8 ]; do
    cat "$MOUNT/file" >"$BASE/reader.$i" 2>/dev/null &
    reader_pids="$reader_pids $!"
    i=$((i + 1))
done
for pid in $reader_pids; do
    wait "$pid" || reader_failures=$((reader_failures + 1))
done
i=1
while [ "$i" -le 8 ]; do
    [ "$(cat "$BASE/reader.$i")" = "contract-data" ] ||
        reader_failures=$((reader_failures + 1))
    i=$((i + 1))
done
tap_ok "$reader_failures" "same-node concurrent readers return identical data"
stop_daemon

printf 'USFS-SCENARIO 1\nLOOKUP delay 5000 1\n' >"$SCENARIO"
start_daemon >/dev/null 2>&1
cat "$MOUNT/file" >"$BASE/disconnect" 2>/dev/null &
WAIT_PID=$!
usfs_track_pid "$WAIT_PID"
usfs_wait_until "in-flight request before disconnect" 10 request_seen
kill -TERM "$DAEMON_PID" 2>/dev/null
if usfs_wait_until "waiter wake after daemon close" 15 \
   sh -c "! ps -p $WAIT_PID >/dev/null 2>&1"; then
    wait "$WAIT_PID"; WAIT_RC=$?
    if [ "$WAIT_RC" -ne 0 ]; then
        tap_ok 0 "daemon close wakes in-flight filesystem caller with error"
    else
        tap_ok 1 "daemon close wakes in-flight filesystem caller with error"
    fi
else
    tap_ok 1 "daemon close wakes in-flight filesystem caller with error"
fi
stop_daemon

usfs_namespace_case
tap_ok "$?" "held child stat waits for ancestor rename publication"

if no_usfs_state; then
    tap_ok 0 "concurrency cases leave no mount or daemon state"
else
    tap_ok 1 "concurrency cases leave no mount or daemon state"
fi

tap_finish
