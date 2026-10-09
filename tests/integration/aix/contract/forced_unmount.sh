#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify safe forced recovery, stale-vnode behavior, and clean remount.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/forced_unmount.sh
#
# Notes:
#   UVMNT_FORCE detaches the namespace and quarantines the daemon channel, but
#   storage behind an open file must remain valid until its final close/release.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-forced-unmount.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0
FD_OPEN=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

daemon_exited()
{
    ! kill -0 "$DAEMON_PID" 2>/dev/null
}

close_stale_fd()
{
    # AIX reports the stale vnode's EIO from close(2). That is expected after
    # forced recovery; keep it out of successful TAP output while restoring
    # the coordinator's stderr immediately afterwards.
    exec 4>&2
    exec 2>/dev/null
    exec 3<&-
    exec 2>&4
    exec 4>&-
    FD_OPEN=0
}

cleanup()
{
    if [ "$FD_OPEN" -eq 1 ]; then
        close_stale_fd
    fi
    usfs_owned_cleanup
    rm -rf "$BASE"
}

trap cleanup 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 10

/usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
    >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"
usfs_track_mount "$MOUNT"

if usfs_wait_until "forced-unmount contract mount" 30 is_mounted; then
    tap_ok 0 "forced-unmount scenario daemon mounts"
else
    tap_ok 1 "forced-unmount scenario daemon mounts"
    tap_ok 1 "forced unmount detaches while a vnode is open"
    tap_ok 1 "stale open descriptor fails safely"
    tap_ok 1 "forced recovery terminates the quarantined daemon channel"
    tap_ok 1 "a fresh daemon remount is usable"
    tap_ok 1 "ordinary unmount succeeds after recovery"
    tap_ok 1 "forced removal leaves replacement ownership isolated during old cleanup"
    tap_ok 1 "root metadata error fails closed and permits forced recovery"
    tap_ok 1 "malformed root metadata fails closed and permits forced recovery"
    tap_ok 1 "concurrent final unmaps release stale cached nodes and permit remount"
    tap_finish
    exit $?
fi

if exec 3<"$MOUNT/file"; then
    FD_OPEN=1
fi

if [ "$FD_OPEN" -eq 1 ] && umount -f "$MOUNT" >"$BASE/force.log" 2>&1 &&
   ! is_mounted; then
    tap_ok 0 "forced unmount detaches while a vnode is open"
else
    sed 's/^/# /' "$BASE/force.log" 2>/dev/null
    tap_ok 1 "forced unmount detaches while a vnode is open"
fi

if [ "$FD_OPEN" -eq 1 ] && ! dd bs=1 count=1 <&3 >/dev/null 2>&1; then
    tap_ok 0 "stale open descriptor fails safely"
else
    tap_ok 1 "stale open descriptor fails safely"
fi

if usfs_wait_until "forced-unmount daemon exit" 15 daemon_exited; then
    tap_ok 0 "forced recovery terminates the quarantined daemon channel"
else
    tap_ok 1 "forced recovery terminates the quarantined daemon channel"
fi

if [ "$FD_OPEN" -eq 1 ]; then
    close_stale_fd
fi

/usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
    >>"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"

if usfs_wait_until "forced-unmount recovery mount" 30 is_mounted &&
   cat "$MOUNT/file" >/dev/null 2>&1; then
    tap_ok 0 "a fresh daemon remount is usable"
else
    tap_ok 1 "a fresh daemon remount is usable"
fi

if umount "$MOUNT" >/dev/null 2>&1 && ! is_mounted; then
    tap_ok 0 "ordinary unmount succeeds after recovery"
else
    tap_ok 1 "ordinary unmount succeeds after recovery"
fi

owner_failure=0
usfs_mount_owner_case forced 1 || owner_failure=1
tap_ok "$owner_failure" "forced removal leaves replacement ownership isolated during old cleanup"

for fault in error malformed; do
    failure=0
    rm -f "$BASE/root-fault"
    # Parent directories remain searchable; denial must come from USFS.
    chmod 755 "$BASE" "$MOUNT"
    USFS_SCENARIO_ROOT_FAULT_FILE="$BASE/root-fault" \
    USFS_SCENARIO_RESTRICTED_ROOT=1 \
        /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_wait_until "restricted root mount" 30 is_mounted || failure=1
    if [ "$failure" -eq 0 ]; then
        /usr/sbin/usfs_io_probe stat "$MOUNT" >"$BASE/stat.log" 2>&1 || failure=1
        grep -q '^result=0 errno=0$' "$BASE/stat.log" || failure=1
        su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1 && failure=1
        printf '%s\n' "$fault" >"$BASE/root-fault"
        /usr/sbin/usfs_io_probe stat "$MOUNT" >>"$BASE/stat.log" 2>&1 || failure=1
        grep -q '^result=-1 errno=5$' "$BASE/stat.log" || failure=1
        su nobody -c "cat '$MOUNT/file'" >/dev/null 2>&1 && failure=1
        umount -f "$MOUNT" >"$BASE/force.log" 2>&1 || failure=1
        is_mounted && failure=1
        usfs_wait_until "root failure forced recovery" 15 daemon_exited || failure=1
    fi
    if [ "$failure" -ne 0 ]; then
        cat "$BASE/stat.log" "$BASE/force.log" "$DAEMON_LOG" 2>/dev/null
        umount -f "$MOUNT" >/dev/null 2>&1
        kill -TERM "$DAEMON_PID" 2>/dev/null
    fi
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
    tap_ok "$failure" "$fault root metadata fails stat and unauthorized traversal while permitting forced recovery"
done

pair_ready()
{
    test -f "$BASE/ready-read" && test -f "$BASE/ready-write"
}
pair_done()
{
    ! kill -0 "$READ_PID" 2>/dev/null && ! kill -0 "$WRITE_PID" 2>/dev/null
}
no_stale_mounts()
{
    /usr/sbin/usfsctl >"$BASE/status.log" 2>&1 &&
        grep -q '^mounts: 0 active, 0 stale$' "$BASE/status.log"
}
pair_failure=0
for iteration in 1 2 3 4; do
    echo "# concurrent stale unmap iteration $iteration"
    rm -f "$BASE/ready-read" "$BASE/ready-write" "$BASE/release"
    /usr/sbin/usfs_memfs -f "$MOUNT" >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    if ! usfs_wait_until "cached pair mount" 30 is_mounted; then pair_failure=1; break; fi
    dd if=/dev/zero of="$MOUNT/first" bs=4096 count=1 >/dev/null 2>&1 || pair_failure=1
    dd if=/dev/zero of="$MOUNT/second" bs=4096 count=1 >/dev/null 2>&1 || pair_failure=1
    /usr/sbin/usfs_io_probe mmap-release-read "$MOUNT/first" 4096 0 \
        "$BASE/ready-read" "$BASE/release" >"$BASE/read.log" 2>&1 &
    READ_PID=$!
    usfs_track_pid "$READ_PID"
    /usr/sbin/usfs_io_probe mmap-release-write "$MOUNT/second" 4096 0 \
        "$BASE/ready-write" "$BASE/release" >"$BASE/write.log" 2>&1 &
    WRITE_PID=$!
    usfs_track_pid "$WRITE_PID"
    usfs_wait_until "both cached mappings with closed descriptors" 20 pair_ready || pair_failure=1
    umount -f "$MOUNT" >"$BASE/force.log" 2>&1 || pair_failure=1
    /usr/sbin/usfsctl >"$BASE/held-status.log" 2>&1
    grep -q '^mounts: 0 active, 1 stale$' "$BASE/held-status.log" || pair_failure=1
    touch "$BASE/release"
    if ! usfs_wait_until "concurrent cached unmaps" 30 pair_done; then
        pair_failure=1
        kill -KILL "$READ_PID" "$WRITE_PID" 2>/dev/null
        break
    fi
    wait "$READ_PID" || pair_failure=1
    wait "$WRITE_PID" || pair_failure=1
    grep -q '^result=0 stage=release errno=0$' "$BASE/read.log" || pair_failure=1
    grep -q '^result=0 stage=release errno=0$' "$BASE/write.log" || pair_failure=1
    if ! usfs_wait_until "cached pair daemon exit" 15 daemon_exited; then
        pair_failure=1
        kill -KILL "$DAEMON_PID" 2>/dev/null
        break
    fi
    wait "$DAEMON_PID" 2>/dev/null
    usfs_wait_until "cached pair resources released" 15 no_stale_mounts || pair_failure=1
    [ "$pair_failure" -eq 0 ] || break
done
if [ "$pair_failure" -ne 0 ]; then
    cat "$BASE/read.log" "$BASE/write.log" "$BASE/status.log" "$DAEMON_LOG" 2>/dev/null
fi
tap_ok "$pair_failure" "concurrent final unmaps release stale cached nodes and permit remount"

tap_finish
