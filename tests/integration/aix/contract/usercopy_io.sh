#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify invalid user addresses on READ and WRITE return EFAULT at the right
#   phase, preserve request accounting, and leave the channel usable.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/usercopy_io.sh
#
# Notes:
#   Requires USFS_TESTING, usfs_usercopy_test, and the scenario daemon. Request
#   logs distinguish failures before daemon delivery from reply-copy failures.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-usercopy.$$
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
    usfs_wait_until "user-copy mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

opcode_count()
{
    awk -v opcode="$1" '$1 == opcode { count++ } END { print count+0 }' \
        "$REQUESTS" 2>/dev/null
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 9

if start_daemon; then
    tap_ok 0 "user-copy scenario daemon mounts"
else
    tap_ok 1 "user-copy scenario daemon mounts"
fi

if /usr/sbin/usfs_usercopy_test read "$MOUNT/file" \
   >"$BASE/read.log" 2>&1; then
    tap_ok 0 "read rejects an invalid user destination with EFAULT"
else
    sed 's/^/# /' "$BASE/read.log"
    tap_ok 1 "read rejects an invalid user destination with EFAULT"
fi

if [ "$(opcode_count 4)" -gt 0 ]; then
    tap_ok 0 "read copyout failure occurs after a daemon reply"
else
    tap_ok 1 "read copyout failure occurs after a daemon reply"
fi

content=$(cat "$MOUNT/file" 2>/dev/null)
tap_is "$content" "contract-data" "read path recovers after copyout failure"

writes_before=$(opcode_count 17)
if /usr/sbin/usfs_usercopy_test write "$MOUNT/file" \
   >"$BASE/write.log" 2>&1; then
    tap_ok 0 "write rejects an invalid user source with EFAULT"
else
    sed 's/^/# /' "$BASE/write.log"
    tap_ok 1 "write rejects an invalid user source with EFAULT"
fi
writes_after=$(opcode_count 17)
tap_is "$writes_after" "$writes_before" \
    "write copyin failure is rejected before daemon delivery"

if printf 'x' >"$MOUNT/file" 2>>"$DAEMON_LOG"; then
    tap_ok 0 "write path recovers after copyin failure"
else
    tap_ok 1 "write path recovers after copyin failure"
fi

if [ "$(opcode_count 17)" -gt "$writes_after" ]; then
    tap_ok 0 "recovery write reaches the daemon exactly through the normal path"
else
    tap_ok 1 "recovery write reaches the daemon exactly through the normal path"
fi

stop_daemon
if /usr/sbin/usfs_testctl status >/dev/null 2>&1; then
    tap_ok 0 "user-copy failures leave lifecycle admission usable"
else
    tap_ok 1 "user-copy failures leave lifecycle admission usable"
fi

tap_finish
