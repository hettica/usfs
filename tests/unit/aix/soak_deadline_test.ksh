#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify that bounded soak commands preserve their own exit status on AIX.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.

. "${USFS_SOURCE_DIR:?}/tests/soak/lib/common.ksh"

TMPDIR=/tmp/usfs-soak-deadline-test.$$
export TMPDIR
mkdir "$TMPDIR" || exit 1
trap 'rm -rf "$TMPDIR"' EXIT HUP INT TERM

for expected_rc in 0 1 7; do
    iteration=0
    while [ "$iteration" -lt 8 ]; do
        soak_run_with_deadline 5 /usr/bin/ksh -c "exit $expected_rc"
        actual_rc=$?
        if [ "$actual_rc" -ne "$expected_rc" ]; then
            echo "soak deadline returned $actual_rc for command exit $expected_rc" >&2
            exit 1
        fi
        iteration=$((iteration + 1))
    done
done

soak_run_with_deadline 5 /usr/bin/ksh -c 'echo stdout; echo stderr >&2; exit 7' \
    >"$TMPDIR/output" 2>&1
actual_rc=$?
if [ "$actual_rc" -ne 7 ] || ! grep -q stdout "$TMPDIR/output" ||
   ! grep -q stderr "$TMPDIR/output"; then
    echo "soak deadline lost output or the failing command status" >&2
    exit 1
fi

(
    soak_run_with_deadline 5 /usr/bin/ksh -c 'exit 1'
    printf '%s\n' "$?" >"$TMPDIR/first.status"
) &
first_pid=$!
(
    soak_run_with_deadline 5 /usr/bin/ksh -c 'exit 7'
    printf '%s\n' "$?" >"$TMPDIR/second.status"
) &
second_pid=$!
wait "$first_pid" "$second_pid"

if [ "$(cat "$TMPDIR/first.status")" -ne 1 ] ||
   [ "$(cat "$TMPDIR/second.status")" -ne 7 ]; then
    echo "concurrent soak deadlines mixed command statuses" >&2
    exit 1
fi

soak_run_with_deadline 5 /usr/bin/ksh -c 'kill -TERM $$' >/dev/null 2>&1
actual_rc=$?
if [ "$actual_rc" -eq 0 ] || [ "$actual_rc" -eq 124 ]; then
    echo "soak deadline lost a command terminated by a signal" >&2
    exit 1
fi

soak_run_with_deadline 1 /usr/bin/ksh -c 'sleep 30' >/dev/null 2>&1
if [ "$?" -ne 124 ]; then
    echo "soak deadline did not report a timed-out command" >&2
    exit 1
fi

if [ -n "$(ls -A "$TMPDIR")" ]; then
    # The test's own result files are retained until the trap runs.
    for entry in "$TMPDIR"/*; do
        case "$entry" in
            "$TMPDIR/output"|"$TMPDIR/first.status"|"$TMPDIR/second.status") ;;
            *) echo "soak deadline left a command status directory: $entry" >&2; exit 1 ;;
        esac
    done
fi

echo "ok - bounded soak commands preserve exit status and timeout cleanup"
