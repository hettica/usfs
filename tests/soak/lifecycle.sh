#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Qualify disruptive lifecycle recovery separately from the normal campaign.
# Usage: ./lifecycle.sh CYCLES
# Never run beside script.sh.

set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/lib/common.ksh"

[ "$#" -eq 1 ] || { echo "usage: ./lifecycle.sh CYCLES" >&2; exit 2; }
soak_is_uint "$1" && [ "$1" -ge 1 ] && [ "$1" -le 100 ] || {
    echo "CYCLES must be an integer from 1 to 100" >&2; exit 2;
}
[ "$(id -u)" -eq 0 ] || { echo "root is required" >&2; exit 2; }
/usr/sbin/lsdev -l usfs0 2>/dev/null | grep -q Available || {
    echo "usfs0 is not Available" >&2; exit 2;
}

BASE=${USFS_LIFECYCLE_WORKDIR:-/var/tmp/usfs-lifecycle.$$}
mkdir -p "$BASE" || exit 2
ORIGINAL=$(/usr/sbin/chusfs -l usfs0) || exit 2
PIDS=""
MOUNTS=""

restore_config()
{
    _request=$(echo "$ORIGINAL" | awk -F= '$1 == "request_timeout_ms" {print $2}')
    _pager=$(echo "$ORIGINAL" | awk -F= '$1 == "pager_timeout_ms" {print $2}')
    _queue=$(echo "$ORIGINAL" | awk -F= '$1 == "max_outstanding_requests" {print $2}')
    /usr/sbin/chusfs -l usfs0 -a request_timeout_ms="$_request" \
        -a pager_timeout_ms="$_pager" -a max_outstanding_requests="$_queue" \
        >/dev/null 2>&1
}

cleanup()
{
    for _mount in $MOUNTS; do
        soak_is_mounted "$_mount" && umount "$_mount" >/dev/null 2>&1
    done
    for _pid in $PIDS; do soak_bounded_kill "$_pid" TERM; done
    /usr/sbin/lsdev -l usfs0 2>/dev/null | grep -q Available || mkdev -l usfs0 >/dev/null 2>&1
    restore_config
}
trap cleanup 0 HUP INT TERM

CYCLE=1
while [ "$CYCLE" -le "$1" ]; do
    MOUNT="$BASE/active.$CYCLE"
    mkdir -p "$MOUNT"
    "$ROOT/bin/usfs_memfs" --size=16 --inodes=1000 "$MOUNT" >"$BASE/active.$CYCLE.log" 2>&1 &
    PID=$!; PIDS="$PIDS $PID"; MOUNTS="$MOUNTS $MOUNT"
    soak_wait_mounted "$MOUNT" 60 || exit 1
    if soak_run_with_deadline 30 rmdev -l usfs0 >"$BASE/active-rmdev.$CYCLE.log" 2>&1; then
        echo "rmdev unexpectedly succeeded with an active mount" >&2
        exit 1
    fi
    /usr/sbin/lsdev -l usfs0 | grep -q Available || exit 1
    soak_run_with_deadline 30 umount "$MOUNT" || exit 1
    soak_bounded_kill "$PID" TERM || exit 1
    PIDS=""; MOUNTS=""

    # Exercise daemon death and bounded recovery. Forced unmount is confined
    # to this explicitly disruptive script and used only if ordinary cleanup
    # cannot remove the disconnected mount.
    MOUNT="$BASE/death.$CYCLE"; mkdir -p "$MOUNT"
    "$ROOT/bin/usfs_memfs" --size=16 --inodes=1000 "$MOUNT" >"$BASE/death.$CYCLE.log" 2>&1 &
    PID=$!; PIDS="$PID"; MOUNTS="$MOUNT"
    soak_wait_mounted "$MOUNT" 60 || exit 1
    soak_bounded_kill "$PID" KILL || exit 1
    PIDS=""
    if ! soak_run_with_deadline 30 umount "$MOUNT" >/dev/null 2>&1; then
        echo "cycle=$CYCLE forced_recovery=1" >>"$BASE/lifecycle.log"
        soak_run_with_deadline 30 umount -f "$MOUNT" || exit 1
    fi
    MOUNTS=""

    # A delayed reply beyond the temporary request deadline must quarantine
    # only that channel; a fresh channel must recover immediately afterward.
    /usr/sbin/chusfs -l usfs0 -a request_timeout_ms=500 >/dev/null || exit 1
    printf 'USFS-SCENARIO 1\nLOOKUP delay 2000\n' >"$BASE/timeout.scn"
    MOUNT="$BASE/timeout.$CYCLE"; mkdir -p "$MOUNT"
    "$ROOT/bin/usfs_scenario_daemon" "$MOUNT" "$BASE/timeout.scn" \
        "$BASE/timeout.requests" >"$BASE/timeout.$CYCLE.log" 2>&1 &
    PID=$!; PIDS="$PID"; MOUNTS="$MOUNT"
    soak_wait_mounted "$MOUNT" 60 || exit 1
    soak_run_with_deadline 15 ls "$MOUNT/file" >/dev/null 2>&1
    soak_run_with_deadline 30 umount "$MOUNT" >/dev/null 2>&1 ||
        soak_run_with_deadline 30 umount -f "$MOUNT" || exit 1
    soak_bounded_kill "$PID" TERM; PIDS=""; MOUNTS=""
    restore_config || exit 1

    RECOVERY="$BASE/recovery.$CYCLE"; mkdir -p "$RECOVERY"
    "$ROOT/bin/usfs_memfs" --size=16 --inodes=1000 "$RECOVERY" >"$BASE/recovery.$CYCLE.log" 2>&1 &
    PID=$!; PIDS="$PID"; MOUNTS="$RECOVERY"
    soak_wait_mounted "$RECOVERY" 60 || exit 1
    echo recovery >"$RECOVERY/file" || exit 1
    cat "$RECOVERY/file" >/dev/null || exit 1
    soak_run_with_deadline 30 umount "$RECOVERY" || exit 1
    soak_bounded_kill "$PID" TERM; PIDS=""; MOUNTS=""

    rmdev -l usfs0 >/dev/null 2>&1 || exit 1
    /usr/lib/methods/usfs/ucfgusfs -l usfs0 >/dev/null 2>&1 || exit 1
    [ "$(lsdev -l usfs0 -F status 2>/dev/null)" = Defined ] || exit 1
    mkdev -l usfs0 >/dev/null 2>&1 || exit 1
    mkdev -l usfs0 >/dev/null 2>&1 || exit 1
    [ "$(lsdev -l usfs0 -F status 2>/dev/null)" = Available ] || exit 1
    "$ROOT/bin/usfs_admin_probe" --safe >/dev/null || exit 1
    echo "lifecycle cycle $CYCLE passed"
    CYCLE=$((CYCLE + 1))
done

rm -rf "$BASE"
trap - 0 HUP INT TERM
echo "isolated lifecycle qualification passed: $1 cycles"
exit 0
