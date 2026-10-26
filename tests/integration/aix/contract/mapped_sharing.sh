#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Compare actual open/mmap/close/munmap sharing behavior with JFS2, including
#   a writable O_RSHARE owner and a mapping that outlives its descriptor.
# Usage:
#   Run through `make test`, or on a configured USFS host with the scenario
#   daemon installed: USFS_SOURCE_DIR=/path/to/usfs bash this-script

set -u
. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-mapped-sharing.$$
MOUNT=$BASE/mnt
DAEMON_PID=0
PROBE_PID=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

process_exited()
{
    ! kill -0 "$1" 2>/dev/null
}

cleanup()
{
    local status=$?
    trap - EXIT HUP INT TERM
    if [[ "$PROBE_PID" -ne 0 ]]; then
        kill -KILL "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
    if is_mounted; then
        umount "$MOUNT" || umount -f "$MOUNT" || status=1
    fi
    if [[ "$DAEMON_PID" -ne 0 ]]; then
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
        if ! usfs_wait_until "sharing daemon cleanup" 10 process_exited "$DAEMON_PID"; then
            kill -KILL "$DAEMON_PID" 2>/dev/null || true
            status=1
        fi
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    if [[ "$status" -eq 0 ]]; then
        rm -rf "$BASE"
    else
        tap_diag "sharing evidence retained in $BASE"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

run_probe()
{
    local path=$1
    local output=$2
    "$BASE/probe" "$path" >"$output" 2>&1 &
    PROBE_PID=$!
    if ! usfs_wait_until "mapped sharing probe" 45 process_exited "$PROBE_PID"; then
        kill -KILL "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
        PROBE_PID=0
        return 1
    fi
    local status=0
    wait "$PROBE_PID" || status=1
    PROBE_PID=0
    sed 's/^/# /' "$output"
    return "$status"
}

mkdir -p "$MOUNT" || exit 1
tap_plan 3
gcc -maix64 -Wall -Wextra -Werror -o "$BASE/probe" \
    "$USFS_SOURCE_DIR/tests/e2e/fixtures/mapped_sharing_probe.c" || exit 1
dd if=/dev/zero of="$BASE/control" bs=8192 count=1 >/dev/null 2>&1 || exit 1
run_probe "$BASE/control" "$BASE/jfs2.log"
tap_ok "$?" "JFS2 permits the writable shared owner and excludes sharing until last unmap"

# No backend sharing policy is used: this isolates USFS kernel admission.
printf 'USFS-SCENARIO 1\n' >"$BASE/scenario"
USFS_SCENARIO_FULL_READS=1 USFS_SCENARIO_FILE_SIZE=8192 \
    /usr/sbin/usfs_scenario_daemon "$MOUNT" "$BASE/scenario" "$BASE/requests.log" \
    >"$BASE/daemon.log" 2>&1 &
DAEMON_PID=$!
usfs_wait_until "mapped sharing mount" 30 is_mounted || exit 1
run_probe "$MOUNT/file" "$BASE/usfs.log"
tap_ok "$?" "USFS permits the writable shared owner and excludes sharing until last unmap"
cmp "$BASE/jfs2.log" "$BASE/usfs.log" >/dev/null 2>&1
tap_ok "$?" "USFS mapped sharing results match JFS2"
tap_finish
