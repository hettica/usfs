#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise AIX vnode compatibility and durability behavior: fsync, range
#   fsync, close-time flush, and filesystem sync reach the daemon and
#   propagate errors, while fclear and record locks return ENOSYS.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/vnode_compat.sh
#
# Notes:
#   Requires the test-only usfs_io_probe. Exact errno assertions prevent an
#   unsupported operation from becoming an unsafe partial implementation.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-vnode-compat.$$
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
    USFS_SCENARIO_FORCE_SYNC_RULES=1 \
        /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "vnode compatibility mount" 30 is_mounted
}

value()
{
    printf '%s\n' "$1" |
        awk -v key="$2" '{
            for (i = 1; i <= NF; i++) {
                split($i, pair, "=")
                if (pair[1] == key) {
                    print pair[2]
                    exit
                }
            }
        }'
}

expect_success()
{
    operation=$1
    description=$2
    target=${3:-$MOUNT/file}
    out=$(/usr/sbin/usfs_io_probe "$operation" "$target")
    if [ "$(value "$out" result)" -ge 0 ] &&
       [ "$(value "$out" errno)" -eq 0 ]; then
        tap_ok 0 "$description succeeds after daemon acknowledgement"
    else
        tap_diag "$out"
        tap_ok 1 "$description succeeds after daemon acknowledgement"
    fi
}

expect_eio()
{
    operation=$1
    description=$2
    target=${3:-$MOUNT/file}
    out=$(/usr/sbin/usfs_io_probe "$operation" "$target")
    if [ "$(value "$out" result)" -eq -1 ] &&
       [ "$(value "$out" errno)" -eq "$(value "$out" EIO)" ]; then
        tap_ok 0 "$description propagates daemon EIO"
    else
        tap_diag "$out"
        tap_ok 1 "$description propagates daemon EIO"
    fi
}

expect_unsupported()
{
    operation=$1
    description=$2
    out=$(/usr/sbin/usfs_io_probe "$operation" "$MOUNT/file")
    if [ "$(value "$out" result)" -eq -1 ] &&
       [ "$(value "$out" errno)" -eq "$(value "$out" ENOSYS)" ]; then
        tap_ok 0 "$description fails explicitly with ENOSYS"
    else
        tap_diag "$out"
        tap_ok 1 "$description fails explicitly with ENOSYS"
    fi
}

expect_request_count()
{
    opcode=$1
    expected=$2
    description=$3
    actual=$(grep -c "^$opcode " "$REQUESTS")
    if [ "$actual" -eq "$expected" ]; then
        tap_ok 0 "$description"
    else
        tap_diag "opcode=$opcode expected=$expected actual=$actual"
        tap_ok 1 "$description"
    fi
}

expect_force_sync_request_count()
{
    expected=$1
    description=$2
    actual=$(grep -c '^DETAIL 20 1$' "$REQUESTS")

    if [ "$actual" -eq "$expected" ]; then
        tap_ok 0 "$description"
    else
        tap_diag "force sync requests expected=$expected actual=$actual"
        tap_ok 1 "$description"
    fi
}

expect_log_line()
{
    line=$1
    description=$2
    if grep -q "^$line\$" "$REQUESTS"; then
        tap_ok 0 "$description"
    else
        tap_diag "missing log line: $line"
        tap_diag "durability details: $(grep '^DETAIL 1[89] ' "$REQUESTS" | tr '\n' ';')"
        tap_ok 1 "$description"
    fi
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
printf 'FSYNC error 5 3\n' >>"$SCENARIO"
printf 'FLUSH error 5 4\n' >>"$SCENARIO"
printf 'SYNCFS error 5 2\n' >>"$SCENARIO"
printf 'SYNCFS error 5 4\n' >>"$SCENARIO"
tap_plan 20

if start_daemon; then
    expect_success fsync "whole-file fsync"
    expect_success fsync-range "range fsync"
    expect_eio fsync "whole-file fsync failure"
    expect_eio close "close-time flush failure"
    expect_success syncfs-try "TRY sync before first FORCE" "$MOUNT"
    expect_success syncfs "filesystem sync" "$MOUNT"
    expect_success syncfs-try "TRY sync before failed FORCE" "$MOUNT"
    expect_eio syncfs "filesystem sync failure" "$MOUNT"
    expect_success syncfs-try "TRY sync before type FORCE" "$MOUNT"
    expect_success syncfs-type "filesystem-type sync" "$MOUNT"
    expect_success syncfs-try "TRY sync before failed type FORCE" "$MOUNT"
    expect_eio syncfs-type "filesystem-type sync failure" "$MOUNT"
    expect_unsupported fclear "AIX fclear"
    expect_unsupported lock "record locking"

    expect_request_count 19 3 \
        "exactly three fsync requests reach the daemon"
    expect_request_count 18 6 \
        "every file close sends a flush request"
    # AIX may issue periodic TRY-mode syncs. The probe sends four FORCE-mode
    # syncs, which are the requests this test owns.
    expect_force_sync_request_count 4 \
        "specific and type-wide filesystem sync requests reach the daemon"
    expect_log_line "DETAIL 19 [0-9][0-9]* 0 0 2" \
        "whole-file fsync body carries the handle and through-EOF range"
    expect_log_line "DETAIL 19 [0-9][0-9]* 0 1 2" \
        "range fsync body carries offset, length, and range flag"
    expect_log_line "DETAIL 20 1" \
        "filesystem sync body carries normalized force mode"
else
    tap_ok 1 "whole-file fsync succeeds after daemon acknowledgement"
    tap_ok 1 "range fsync succeeds after daemon acknowledgement"
    tap_ok 1 "whole-file fsync failure propagates daemon EIO"
    tap_ok 1 "close-time flush failure propagates daemon EIO"
    tap_ok 1 "TRY sync before first FORCE succeeds after daemon acknowledgement"
    tap_ok 1 "filesystem sync succeeds after daemon acknowledgement"
    tap_ok 1 "TRY sync before failed FORCE succeeds after daemon acknowledgement"
    tap_ok 1 "filesystem sync failure propagates daemon EIO"
    tap_ok 1 "TRY sync before type FORCE succeeds after daemon acknowledgement"
    tap_ok 1 "filesystem-type sync succeeds after daemon acknowledgement"
    tap_ok 1 "TRY sync before failed type FORCE succeeds after daemon acknowledgement"
    tap_ok 1 "filesystem-type sync failure propagates daemon EIO"
    tap_ok 1 "AIX fclear fails explicitly with ENOSYS"
    tap_ok 1 "record locking fails explicitly with ENOSYS"
    tap_ok 1 "exactly three fsync requests reach the daemon"
    tap_ok 1 "every file close sends a flush request"
    tap_ok 1 "specific and type-wide filesystem sync requests reach the daemon"
    tap_ok 1 "whole-file fsync body carries the handle and through-EOF range"
    tap_ok 1 "range fsync body carries offset, length, and range flag"
    tap_ok 1 "filesystem sync body carries normalized force mode"
fi

tap_finish
