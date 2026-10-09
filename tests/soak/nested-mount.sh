#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Qualify nested USFS mounts separately from the normal campaign.
# Usage: ./nested-mount.sh ITERATIONS

set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/lib/common.ksh"

[ "$#" -eq 1 ] || { echo "usage: ./nested-mount.sh ITERATIONS" >&2; exit 2; }
soak_is_uint "$1" && [ "$1" -ge 1 ] && [ "$1" -le 100 ] || {
    echo "ITERATIONS must be an integer from 1 to 100" >&2; exit 2;
}
[ "$(id -u)" -eq 0 ] || { echo "root is required" >&2; exit 2; }
/usr/sbin/lsdev -l usfs0 2>/dev/null | grep -q Available || {
    echo "usfs0 is not Available" >&2; exit 2;
}

BASE=${USFS_NESTED_WORKDIR:-/var/tmp/usfs-nested.$$}
mkdir -p "$BASE" || exit 2
LEFTOVERS=""

cleanup()
{
    [ -n "${CHILD_MOUNT:-}" ] && soak_is_mounted "$CHILD_MOUNT" && umount "$CHILD_MOUNT" >/dev/null 2>&1
    [ -n "${PARENT_MOUNT:-}" ] && soak_is_mounted "$PARENT_MOUNT" && umount "$PARENT_MOUNT" >/dev/null 2>&1
    [ -n "${CHILD_PID:-}" ] && soak_bounded_kill "$CHILD_PID" TERM
    [ -n "${PARENT_PID:-}" ] && soak_bounded_kill "$PARENT_PID" TERM
    [ -n "$LEFTOVERS" ] || rm -rf "$BASE"
}
trap cleanup 0 HUP INT TERM

ITERATION=1
while [ "$ITERATION" -le "$1" ]; do
    PARENT_MOUNT="$BASE/parent.$ITERATION"
    mkdir -p "$PARENT_MOUNT"
    "$ROOT/bin/usfs_memfs" --size=16 --inodes=1000 "$PARENT_MOUNT" \
        >"$BASE/parent.$ITERATION.log" 2>&1 &
    PARENT_PID=$!
    soak_wait_mounted "$PARENT_MOUNT" 60 || { echo "parent mount failed" >&2; exit 1; }
    echo "parent-$ITERATION" >"$PARENT_MOUNT/parent-file" || exit 1
    CHILD_MOUNT="$PARENT_MOUNT/child"
    mkdir "$CHILD_MOUNT" || exit 1
    "$ROOT/bin/usfs_memfs" --size=16 --inodes=1000 "$CHILD_MOUNT" \
        >"$BASE/child.$ITERATION.log" 2>&1 &
    CHILD_PID=$!
    soak_wait_mounted "$CHILD_MOUNT" 60 || { echo "child mount failed" >&2; exit 1; }
    echo nested-usfs >"$CHILD_MOUNT/child-file" || exit 1
    [ "$(cat "$CHILD_MOUNT/child-file")" = nested-usfs ] || exit 1

    # The parent must remain busy while a child filesystem is mounted on it.
    if soak_run_with_deadline 20 umount "$PARENT_MOUNT" >/dev/null 2>&1; then
        echo "parent unmount unexpectedly succeeded with active child" >&2
        exit 1
    fi
    soak_is_mounted "$PARENT_MOUNT" && soak_is_mounted "$CHILD_MOUNT" || exit 1

    soak_run_with_deadline 30 umount "$CHILD_MOUNT" || { LEFTOVERS="$CHILD_MOUNT"; exit 1; }
    soak_bounded_kill "$CHILD_PID" TERM || { LEFTOVERS="$CHILD_PID"; exit 1; }
    CHILD_PID=""
    [ "$(cat "$PARENT_MOUNT/parent-file")" = "parent-$ITERATION" ] || exit 1
    echo recovery >"$PARENT_MOUNT/recovery" || exit 1
    soak_run_with_deadline 30 umount "$PARENT_MOUNT" || { LEFTOVERS="$PARENT_MOUNT"; exit 1; }
    soak_bounded_kill "$PARENT_PID" TERM || { LEFTOVERS="$PARENT_PID"; exit 1; }
    PARENT_PID=""
    PARENT_MOUNT=""
    CHILD_MOUNT=""
    echo "nested iteration $ITERATION passed"
    ITERATION=$((ITERATION + 1))
done

echo "nested USFS qualification passed: $1 iterations"
exit 0
