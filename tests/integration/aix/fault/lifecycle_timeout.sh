#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify bounded timeout cleanup for unmatched mount and channel rendezvous,
#   including reservation release, fault accounting, and admission recovery.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/fault/lifecycle_timeout.sh
#
# Notes:
#   Requires USFS_TESTING timeout controls. The exit trap resets fault state,
#   terminates only tracked processes, and removes only this test's workspace.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-lifecycle-timeout.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0

daemon_exited()
{
    ! ps -p "$DAEMON_PID" >/dev/null 2>&1
}

cleanup()
{
    usfs_owned_cleanup
    if [ "$DAEMON_PID" -ne 0 ]; then
        kill -TERM "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
    fi
    /usr/sbin/usfs_testctl reset >/dev/null 2>&1
    rm -rf "$BASE"
}

trap cleanup 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 6

failure=0
/usr/sbin/usfs_testctl schedule 1 >/dev/null 2>&1 || failure=1
start=$(date +%s)
/usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
    >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"
if ! usfs_wait_until "unpaired mount rendezvous timeout" 15 daemon_exited; then
    failure=1
fi
wait "$DAEMON_PID"; DAEMON_RC=$?
elapsed=$(( $(date +%s) - start ))
DAEMON_PID=0
if [ "$DAEMON_RC" -eq 0 ] || [ "$elapsed" -gt 14 ]; then
    failure=1
fi
tap_ok "$failure" "unpaired mount rendezvous fails within its bounded deadline"

status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null)
case "$status" in
    *'schedule=1 reached=0x1 '*'timed_out=1 active=0'*)
        tap_ok 0 "mount timeout is recorded and disarmed" ;;
    *)
        tap_diag "mount timeout status: $status"
        tap_ok 1 "mount timeout is recorded and disarmed" ;;
esac

if /usr/sbin/usfs_testctl reset >/dev/null 2>&1 &&
   /usr/sbin/usfs_testctl info >/dev/null 2>&1; then
    tap_ok 0 "mount timeout releases its reservation and connection"
else
    tap_ok 1 "mount timeout releases its reservation and connection"
fi

channel_failure=0
/usr/sbin/usfs_testctl schedule 2 >/dev/null 2>&1 ||
    channel_failure=1
start=$(date +%s)
/usr/sbin/usfs_testctl info >"$BASE/channel-one.log" 2>&1 &
CHANNEL_ONE_PID=$!
usfs_track_pid "$CHANNEL_ONE_PID"
/usr/sbin/usfs_testctl info >"$BASE/channel-two.log" 2>&1 &
CHANNEL_TWO_PID=$!
usfs_track_pid "$CHANNEL_TWO_PID"
wait "$CHANNEL_ONE_PID"; CHANNEL_ONE_RC=$?
wait "$CHANNEL_TWO_PID"; CHANNEL_TWO_RC=$?
elapsed=$(( $(date +%s) - start ))
if [ "$channel_failure" -eq 0 ] &&
   [ "$CHANNEL_ONE_RC" -ne 0 ] && [ "$CHANNEL_TWO_RC" -ne 0 ] &&
   [ "$elapsed" -le 14 ]; then
    tap_ok 0 "unpaired channel rendezvous fails both allocators within its deadline"
else
    tap_diag "channel timeout elapsed=$elapsed rc1=$CHANNEL_ONE_RC rc2=$CHANNEL_TWO_RC"
    tap_ok 1 "unpaired channel rendezvous fails both allocators within its deadline"
fi

status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null)
case "$status" in
    *'schedule=2 reached=0x8 channel_pre=2 channel_rejected=0 timed_out=1 active=0'*)
        tap_ok 0 "channel timeout is recorded without publishing connections" ;;
    *)
        tap_diag "channel timeout status: $status"
        tap_ok 1 "channel timeout is recorded without publishing connections" ;;
esac

if /usr/sbin/usfs_testctl reset >/dev/null 2>&1 &&
   /usr/sbin/usfs_testctl info >/dev/null 2>&1; then
    tap_ok 0 "channel timeout leaves admission usable and quiescent"
else
    tap_ok 1 "channel timeout leaves admission usable and quiescent"
fi

tap_finish
