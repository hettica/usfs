#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify the production health snapshot, request latency accounting,
#   degraded daemon state, persistent errpt record, and recovery behavior.
#
# Usage:
#   Run through `make test-e2e` in the AIX CMake binary directory.
#   On a prepared AIX VM: cd /home && bash tests/e2e/observability.sh

set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-observability.$$
MOUNT=$BASE/mnt
DAEMON_PID=0

is_mounted() { mount 2>/dev/null | grep -q " $MOUNT "; }

cleanup()
{
    if [ "$DAEMON_PID" -ne 0 ]; then
        kill -KILL "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
    fi
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    usfs_owned_cleanup
    rm -rf "$BASE"
}

trap cleanup 0 1 2 15
mkdir -p "$MOUNT"
tap_plan 6

failure=0
idle=$(/usr/sbin/usfsctl 2>/dev/null) || failure=1
echo "$idle" | grep -q '^USFS health: HEALTHY$' || failure=1
echo "$idle" | grep -q '^daemons: 0 active, 0 unhealthy$' || failure=1
tap_ok "$failure" "usfsctl reports a healthy idle extension without counting itself"

/usr/sbin/usfs_memfs --size=16 --inodes=1000 "$MOUNT" >"$BASE/daemon.log" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"
usfs_track_mount "$MOUNT"
failure=0
usfs_wait_until "observability mount" 30 is_mounted || failure=1
echo observed >"$MOUNT/observed"
cat "$MOUNT/observed" >"$BASE/read.out" 2>&1 || failure=1
mounted=$(/usr/sbin/usfsctl 2>/dev/null) || failure=1
echo "$mounted" | grep -q '^mounts: 1 active, 0 stale$' || failure=1
echo "$mounted" | grep -q '^daemons: 1 active, 0 unhealthy$' || failure=1
echo "$mounted" | grep -q '^latency: p50<=' || failure=1
tap_ok "$failure" "mounted I/O appears in daemon, request, and latency metrics"

kill -KILL "$DAEMON_PID" 2>/dev/null
wait "$DAEMON_PID" 2>/dev/null
DAEMON_PID=0
sleep 1
failure=0
/usr/sbin/usfsctl >"$BASE/degraded.out" 2>"$BASE/degraded.err"
rc=$?
[ "$rc" -eq 1 ] || failure=1
grep -q '^USFS health: DEGRADED$' "$BASE/degraded.out" || failure=1
grep -q '^daemons: 0 active, 1 unhealthy$' "$BASE/degraded.out" || failure=1
grep -q '^last fault: daemon-lost ' "$BASE/degraded.out" || failure=1
tap_ok "$failure" "daemon loss leaves an active mount visibly degraded"

failure=0
/usr/bin/errpt -a -j 0B6DD74B >"$BASE/errpt.out" 2>&1 || failure=1
grep -q 'USFS_DAEMON_LOST' "$BASE/errpt.out" || failure=1
grep -q 'usfs0' "$BASE/errpt.out" || failure=1
tap_ok "$failure" "daemon loss is persisted in AIX errpt"

failure=0
umount "$MOUNT" >/dev/null 2>&1 || failure=1
recovered=$(/usr/sbin/usfsctl 2>/dev/null) || failure=1
echo "$recovered" | grep -q '^USFS health: HEALTHY$' || failure=1
echo "$recovered" | grep -q '^mounts: 0 active, 0 stale$' || failure=1
tap_ok "$failure" "ordinary cleanup clears current degradation without erasing history"

failure=0
/usr/sbin/usfsctl status >/dev/null 2>&1
[ "$?" -eq 0 ] || failure=1
tap_ok "$failure" "explicit usfsctl status preserves the health interface"

tap_finish
