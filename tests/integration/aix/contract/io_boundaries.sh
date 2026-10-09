#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify zero, maximum-chunk, multi-chunk, short-transfer, signed-offset,
#   overflow, and READDIR-cookie behavior at the AIX syscall/wire boundary.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/contract/io_boundaries.sh
#
# Notes:
#   Requires the test-only usfs_io_probe and scenario daemon. Request logs are
#   matched exactly to prove chunk sizes, offsets, and absent daemon calls.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-io-boundaries.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0
MAX_DATA=65536
MULTI_COUNT=131089
MAX_OFFSET=9223372036854775807
LAST_OFFSET=9223372036854775806

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

start_daemon()
{
    mode=${1:-normal}
    rm -f "$REQUESTS" "$DAEMON_LOG"
    case "$mode" in
        full-read)
            USFS_SCENARIO_FULL_READS=1 \
                /usr/sbin/usfs_scenario_daemon \
                "$MOUNT" "$SCENARIO" "$REQUESTS" \
                >"$DAEMON_LOG" 2>&1 & ;;
        wide-dir)
            USFS_SCENARIO_WIDE_READDIR=1 \
                /usr/sbin/usfs_scenario_daemon \
                "$MOUNT" "$SCENARIO" "$REQUESTS" \
                >"$DAEMON_LOG" 2>&1 & ;;
        *)
            /usr/sbin/usfs_scenario_daemon \
                "$MOUNT" "$SCENARIO" "$REQUESTS" \
                >"$DAEMON_LOG" 2>&1 & ;;
    esac
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "I/O boundary mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
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

detail_count()
{
    awk -v opcode="$1" '$1 == "DETAIL" && $2 == opcode { count++ }
        END { print count+0 }' "$REQUESTS" 2>/dev/null
}

details_equal()
{
    expected=$1
    actual=$(awk -v opcode="$2" '$1 == "DETAIL" && $2 == opcode {
        print $3, $4
    }' "$REQUESTS" 2>/dev/null)
    [ "$actual" = "$expected" ]
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 16

start_daemon full-read
out=$(/usr/sbin/usfs_io_probe read "$MOUNT/file" 0 0)
if [ "$(value "$out" result)" -eq 0 ] &&
   [ "$(detail_count 4)" -eq 0 ]; then
    tap_ok 0 "zero-length READ completes without a daemon request"
else
    tap_diag "$out"; tap_ok 1 "zero-length READ completes without a daemon request"
fi
stop_daemon

start_daemon full-read
out=$(/usr/sbin/usfs_io_probe read "$MOUNT/file" "$MAX_DATA" 0)
if [ "$(value "$out" result)" -eq "$MAX_DATA" ] &&
   [ "$(value "$out" pattern)" -eq 1 ] &&
   details_equal "0 $MAX_DATA" 4; then
    tap_ok 0 "maximum protocol READ transfers one exact chunk"
else
    tap_diag "$out"; tap_ok 1 "maximum protocol READ transfers one exact chunk"
fi
stop_daemon

start_daemon full-read
out=$(/usr/sbin/usfs_io_probe read "$MOUNT/file" "$MULTI_COUNT" 0)
expected=$(printf '0 %s\n%s %s\n%s 17' \
    "$MAX_DATA" "$MAX_DATA" "$MAX_DATA" "$((MAX_DATA * 2))")
if [ "$(value "$out" result)" -eq "$MULTI_COUNT" ] &&
   [ "$(value "$out" pattern)" -eq 1 ] &&
   details_equal "$expected" 4; then
    tap_ok 0 "large READ is split into two maximum chunks and one tail"
else
    tap_diag "$out"; tap_ok 1 "large READ is split into two maximum chunks and one tail"
fi
stop_daemon

start_daemon
out=$(/usr/sbin/usfs_io_probe read "$MOUNT/file" "$MAX_DATA" 0)
if [ "$(value "$out" result)" -eq 13 ] &&
   details_equal "0 $MAX_DATA" 4; then
    tap_ok 0 "short READ stops at daemon EOF without a second request"
else
    tap_diag "$out"; tap_ok 1 "short READ stops at daemon EOF without a second request"
fi
stop_daemon

start_daemon full-read
out=$(/usr/sbin/usfs_io_probe pread "$MOUNT/file" 1 "$LAST_OFFSET")
if [ "$(value "$out" result)" -eq 1 ] &&
   details_equal "$LAST_OFFSET 1" 4; then
    tap_ok 0 "READ accepts the final representable byte offset"
else
    tap_diag "$out"; tap_ok 1 "READ accepts the final representable byte offset"
fi
stop_daemon

start_daemon full-read
out=$(/usr/sbin/usfs_io_probe pread "$MOUNT/file" 1 "$MAX_OFFSET")
if [ "$(value "$out" result)" -eq -1 ] &&
   [ "$(value "$out" errno)" -eq "$(value "$out" EFBIG)" ] &&
   [ "$(detail_count 4)" -eq 0 ]; then
    tap_ok 0 "overflowing READ range fails before daemon delivery"
else
    tap_diag "$out"; tap_ok 1 "overflowing READ range fails before daemon delivery"
fi
stop_daemon

start_daemon
out=$(/usr/sbin/usfs_io_probe write "$MOUNT/file" 0 0)
if [ "$(value "$out" result)" -eq 0 ] &&
   [ "$(detail_count 17)" -eq 0 ]; then
    tap_ok 0 "zero-length WRITE completes without a daemon request"
else
    tap_diag "$out"; tap_ok 1 "zero-length WRITE completes without a daemon request"
fi
stop_daemon

start_daemon
out=$(/usr/sbin/usfs_io_probe write "$MOUNT/file" "$MAX_DATA" 0)
if [ "$(value "$out" result)" -eq "$MAX_DATA" ] &&
   details_equal "0 $MAX_DATA" 17; then
    tap_ok 0 "maximum protocol WRITE transfers one exact chunk"
else
    tap_diag "$out"; tap_ok 1 "maximum protocol WRITE transfers one exact chunk"
fi
stop_daemon

start_daemon
out=$(/usr/sbin/usfs_io_probe write "$MOUNT/file" "$MULTI_COUNT" 0)
expected=$(printf '0 %s\n%s %s\n%s 17' \
    "$MAX_DATA" "$MAX_DATA" "$MAX_DATA" "$((MAX_DATA * 2))")
if [ "$(value "$out" result)" -eq "$MULTI_COUNT" ] &&
   details_equal "$expected" 17; then
    tap_ok 0 "large WRITE is split into two maximum chunks and one tail"
else
    tap_diag "$out"; tap_ok 1 "large WRITE is split into two maximum chunks and one tail"
fi
stop_daemon

start_daemon
out=$(/usr/sbin/usfs_io_probe pwrite "$MOUNT/file" 1 "$LAST_OFFSET")
if [ "$(value "$out" result)" -eq 1 ] &&
   details_equal "$LAST_OFFSET 1" 17; then
    tap_ok 0 "WRITE accepts the final representable byte offset"
else
    tap_diag "$out"; tap_ok 1 "WRITE accepts the final representable byte offset"
fi
stop_daemon

start_daemon
out=$(/usr/sbin/usfs_io_probe pwrite "$MOUNT/file" 1 "$MAX_OFFSET")
if [ "$(value "$out" result)" -eq -1 ] &&
   [ "$(value "$out" errno)" -eq "$(value "$out" EFBIG)" ] &&
   [ "$(detail_count 17)" -eq 0 ]; then
    tap_ok 0 "overflowing WRITE range fails before daemon delivery"
else
    tap_diag "$out"; tap_ok 1 "overflowing WRITE range fails before daemon delivery"
fi
stop_daemon

printf 'USFS-SCENARIO 1\nWRITE short-write 0 1\n' >"$SCENARIO"
start_daemon
out=$(/usr/sbin/usfs_io_probe write "$MOUNT/file" 100 0)
if [ "$(value "$out" result)" -eq 50 ] &&
   [ "$(value "$out" final)" -eq 50 ] &&
   details_equal "0 100" 17; then
    tap_ok 0 "short WRITE returns the accepted count and resting offset"
else
    tap_diag "$out"; tap_ok 1 "short WRITE returns the accepted count and resting offset"
fi
stop_daemon

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
start_daemon wide-dir
out=$(/usr/sbin/usfs_io_probe readdir "$MOUNT" 0)
if [ "$(value "$out" result)" -eq -1 ] &&
   [ "$(value "$out" errno)" -eq "$(value "$out" EINVAL)" ] &&
   [ "$(detail_count 6)" -eq 0 ]; then
    tap_ok 0 "zero-length READDIR returns AIX EINVAL without a daemon request"
else
    tap_diag "$out"
    tap_ok 1 "zero-length READDIR returns AIX EINVAL without a daemon request"
fi
stop_daemon

start_daemon wide-dir
out=$(/usr/sbin/usfs_io_probe readdir-loop "$MOUNT" "$MAX_DATA")
READDIR_CURSOR_INDEX_BITS=24
cursor_base=$((1 << READDIR_CURSOR_INDEX_BITS))
expected=$(printf '0 %s\n%s %s\n%s %s\n%s %s' \
    "$MAX_DATA" \
    "$((cursor_base + 1))" "$MAX_DATA" \
    "$((cursor_base + 2))" "$MAX_DATA" \
    "$((cursor_base + 3))" "$MAX_DATA")
if [ "$(value "$out" result)" -eq 0 ] &&
   [ "$(value "$out" calls)" -eq 4 ] &&
   [ "$(value "$out" total)" -gt 0 ] &&
   details_equal "$expected" 6; then
    tap_ok 0 "READDIR advances cookies across multiple batches to explicit EOF"
else
    tap_diag "$out"; tap_ok 1 "READDIR advances cookies across multiple batches to explicit EOF"
fi
stop_daemon

printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
start_daemon
pid_failure=0
/usr/sbin/usfs_io_probe read "$MOUNT/file" 1 0 >"$BASE/pid-one" &
pid_one=$!
/usr/sbin/usfs_io_probe read "$MOUNT/file" 1 0 >"$BASE/pid-two" &
pid_two=$!
wait "$pid_one" || pid_failure=1
wait "$pid_two" || pid_failure=1
for output in "$BASE/pid-one" "$BASE/pid-two"; do
    caller=$(sed -n 's/^pid=//p' "$output")
    [ -n "$caller" ] || pid_failure=1
    awk -v caller="$caller" '$1 == 4 && $7 == caller { found=1 } END { exit !found }' "$REQUESTS" || pid_failure=1
done
stop_daemon
tap_ok "$pid_failure" "concurrent ordinary requests carry the originating process PID"

start_daemon wide-dir
retry_failure=0
/usr/sbin/usfs_io_probe readdir-retry "$MOUNT" >"$BASE/retry.out" 2>&1 || retry_failure=1
[ "$retry_failure" -eq 0 ] || tap_diag "$(cat "$BASE/retry.out")"
stop_daemon
tap_ok "$retry_failure" "undersized native directory buffer preserves the cookie for a larger retry"

tap_finish
