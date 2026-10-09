#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Validate fault-control input checks and representative one-shot connection,
#   request, and vnode allocation failures with same-extension recovery.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/fault/runtime.sh
#
# Notes:
#   Requires USFS_TESTING and a quiescent extension when arming faults. Each
#   case checks firing counters and resets global fault state before exit.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-runtime-fault.$$
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
    usfs_wait_until "fault-test mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

fault_fired()
{
    /usr/sbin/usfs_testctl status 2>/dev/null |
        grep -q "observed=$1 fired=1 armed=0"
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 14

if /usr/sbin/usfs_testctl arm 0 1 12 >/dev/null 2>&1; then
    tap_ok 1 "fault id zero is rejected"
else
    tap_ok 0 "fault id zero is rejected"
fi
if /usr/sbin/usfs_testctl arm 8 0 12 >/dev/null 2>&1; then
    tap_ok 1 "zero occurrence is rejected"
else
    tap_ok 0 "zero occurrence is rejected"
fi
if /usr/sbin/usfs_testctl arm 8 1 128 >/dev/null 2>&1; then
    tap_ok 1 "unsupported errno is rejected"
else
    tap_ok 0 "unsupported errno is rejected"
fi

/usr/sbin/usfs_testctl arm 13 1 12 >/dev/null 2>&1
if /usr/sbin/usfs_probe >/dev/null 2>&1; then
    tap_ok 1 "connection allocation fault reaches the caller"
else
    tap_ok 0 "connection allocation fault reaches the caller"
fi
tap_ok "$(fault_fired 1; echo $?)" "connection allocation fault fires exactly once"
tap_ok "$(/usr/sbin/usfs_probe >/dev/null 2>&1; echo $?)" "connection allocation recovers"

# The first request is the root GETATTR needed to authorize directory search;
# fail the following LOOKUP so the filesystem caller observes the allocation
# error rather than the root's deliberate stat fallback.
/usr/sbin/usfs_testctl arm 8 2 12 >/dev/null 2>&1
if start_daemon; then
    if cat "$MOUNT/file" >/dev/null 2>&1; then
        tap_ok 1 "request allocation failure reaches filesystem caller"
    else
        tap_ok 0 "request allocation failure reaches filesystem caller"
    fi
    content=$(cat "$MOUNT/file" 2>/dev/null)
    tap_is "$content" "contract-data" "request path recovers after one-shot failure"
else
    tap_ok 1 "request allocation failure reaches filesystem caller"
    tap_ok 1 "request path recovers after one-shot failure"
fi
stop_daemon
tap_ok "$(fault_fired 2; echo $?)" "request allocation fault records one firing"

/usr/sbin/usfs_testctl arm 12 1 12 >"$BASE/arm-node.log" 2>&1
arm_rc=$?
if [ "$arm_rc" -eq 0 ] && start_daemon; then
    if cat "$MOUNT/file" >/dev/null 2>&1; then
        tap_ok 1 "root node allocation failure reaches filesystem caller"
    else
        tap_ok 0 "root node allocation failure reaches filesystem caller"
    fi
    content=$(cat "$MOUNT/file" 2>/dev/null)
    tap_is "$content" "contract-data" "node creation recovers after one-shot failure"
else
    tap_diag "node fault arm/start failed: arm_rc=$arm_rc"
    sed 's/^/# /' "$BASE/arm-node.log"
    tap_ok 1 "root node allocation failure reaches filesystem caller"
    tap_ok 1 "node creation recovers after one-shot failure"
fi
stop_daemon
tap_ok "$(fault_fired 1; echo $?)" "node allocation fault records one firing"

start_daemon
if /usr/sbin/usfs_testctl arm 8 1 12 >/dev/null 2>&1; then
    tap_ok 1 "fault control refuses an active mount"
else
    tap_ok 0 "fault control refuses an active mount"
fi
stop_daemon

/usr/sbin/usfs_testctl reset >/dev/null 2>&1
status=$(/usr/sbin/usfs_testctl status 2>/dev/null)
case "$status" in
    *'observed=0 fired=0 armed=0'*) tap_ok 0 "reset clears fault state" ;;
    *) tap_diag "status after reset: $status"; tap_ok 1 "reset clears fault state" ;;
esac

tap_finish
