#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Prove canonical vnode identity under racing lookups, balanced references,
#   parent reconstruction after reclamation, and correct dot-dot traversal.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/concurrency/vnode_identity.sh
#
# Notes:
#   Requires USFS_TESTING vnode accounting and rendezvous controls. The final
#   TAP assertion requires zero live vnodes and balanced ownership counters.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-vnode-identity.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

lookup_waiting()
{
    /usr/sbin/usfs_testctl schedule-status 2>/dev/null |
        grep -q 'lookup_post=1'
}

request_count()
{
    awk -v opcode="$1" -v nodeid="$2" \
        '$1 == opcode && $3 == nodeid { count++ }
         END { print count+0 }' "$REQUESTS" 2>/dev/null
}

status_value()
{
    printf '%s\n' "$1" |
        awk -v key="$2" '
            {
                for (i = 1; i <= NF; i++) {
                    split($i, pair, "=")
                    if (pair[1] == key) {
                        print pair[2]
                        found = 1
                    }
                }
            }
            END { if (!found) print -1 }
        '
}

start_daemon()
{
    rm -f "$REQUESTS" "$DAEMON_LOG"
    /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    usfs_track_pid "$DAEMON_PID"
    usfs_track_mount "$MOUNT"
    usfs_wait_until "vnode identity mount" 30 is_mounted
}

stop_daemon()
{
    umount "$MOUNT" >/dev/null 2>&1 || umount -f "$MOUNT" >/dev/null 2>&1
    [ "$DAEMON_PID" -eq 0 ] || kill -TERM "$DAEMON_PID" 2>/dev/null
    [ "$DAEMON_PID" -eq 0 ] || wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=0
}

trap 'usfs_owned_cleanup; rm -rf "$BASE"' 0 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 18

if /usr/sbin/usfs_testctl reset >/dev/null 2>&1; then
    tap_ok 0 "vnode lifecycle accounting resets while quiescent"
else
    tap_ok 1 "vnode lifecycle accounting resets while quiescent"
fi

if /usr/sbin/usfs_testctl schedule 3 >/dev/null 2>&1; then
    tap_ok 0 "same-node lookup rendezvous arms while quiescent"
else
    tap_ok 1 "same-node lookup rendezvous arms while quiescent"
fi

if start_daemon; then
    tap_ok 0 "vnode identity scenario daemon mounts"
else
    tap_ok 1 "vnode identity scenario daemon mounts"
fi

cat "$MOUNT/file" >"$BASE/first" 2>"$BASE/first.err" &
FIRST_PID=$!
usfs_track_pid "$FIRST_PID"
if usfs_wait_until "first post-reply lookup participant" 8 lookup_waiting; then
    tap_ok 0 "first lookup waits after its validated daemon reply"
else
    tap_ok 1 "first lookup waits after its validated daemon reply"
fi

cat "$MOUNT/file" >"$BASE/second" 2>"$BASE/second.err" &
SECOND_PID=$!
usfs_track_pid "$SECOND_PID"
reader_failure=0
wait "$FIRST_PID" || reader_failure=1
wait "$SECOND_PID" || reader_failure=1
[ "$(cat "$BASE/first" 2>/dev/null)" = "contract-data" ] ||
    reader_failure=1
[ "$(cat "$BASE/second" 2>/dev/null)" = "contract-data" ] ||
    reader_failure=1
tap_ok "$reader_failure" "racing same-node lookups both complete correctly"

schedule_status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null)
case "$schedule_status" in
    *'schedule=3 reached=0xc0 '*'timed_out=0 active=0 lookup_post=2 lookup_resolved=2'*)
        tap_ok 0 "lookup rendezvous records exactly two participants" ;;
    *)
        tap_diag "schedule status: $schedule_status"
        tap_ok 1 "lookup rendezvous records exactly two participants" ;;
esac

node_status=$(/usr/sbin/usfs_testctl node-status 2>/dev/null)
lookup_reused=$(status_value "$node_status" lookup_reused)
duplicate_live=$(status_value "$node_status" duplicate_live)
accounting_errors=$(status_value "$node_status" accounting_errors)
if [ "$lookup_reused" -ge 1 ] &&
   [ "$duplicate_live" -eq 0 ] &&
   [ "$accounting_errors" -eq 0 ]; then
    tap_ok 0 "same node id reuses one live vnode without identity violations"
else
    tap_diag "node status after race: $node_status"
    tap_ok 1 "same node id reuses one live vnode without identity violations"
fi

created=$(status_value "$node_status" created)
reclaimed=$(status_value "$node_status" reclaimed)
live=$(status_value "$node_status" live)
if [ "$created" -eq $((reclaimed + live)) ]; then
    tap_ok 0 "live vnode accounting balances during the mounted race"
else
    tap_diag "node status during race: $node_status"
    tap_ok 1 "live vnode accounting balances during the mounted race"
fi

parent_before=$(request_count 1 5)
parent_failure=0
(
    cd "$MOUNT/dir/child" 2>/dev/null || exit 1
    : >"$BASE/child.ready"
    while [ ! -f "$BASE/parent.release" ]; do
        sleep 1
    done
    cd -P .. 2>/dev/null || exit 1
    pwd -P >"$BASE/parent.result"
) &
PARENT_PID=$!
usfs_track_pid "$PARENT_PID"
usfs_wait_until "child cwd with intermediate vnodes released" 8 \
    test -f "$BASE/child.ready" || parent_failure=1

node_status=$(/usr/sbin/usfs_testctl node-status 2>/dev/null)
live=$(status_value "$node_status" live)
if [ "$live" -eq 1 ]; then
    tap_ok 0 "only the held child vnode remains before dot-dot lookup"
else
    tap_diag "node status while child is held: $node_status"
    tap_ok 1 "only the held child vnode remains before dot-dot lookup"
fi

: >"$BASE/parent.release"
wait "$PARENT_PID" || parent_failure=1
got=$(cat "$BASE/parent.result" 2>/dev/null)
if [ "$parent_failure" -eq 0 ] && [ "$got" = "$MOUNT/dir" ]; then
    tap_ok 0 "dot-dot returns the canonical parent directory"
else
    tap_diag "parent traversal returned '$got'"
    tap_ok 1 "dot-dot returns the canonical parent directory"
fi

parent_after=$(request_count 1 5)
if [ "$parent_after" -gt "$parent_before" ]; then
    tap_ok 0 "parent is resolved through the daemon's current namespace"
else
    tap_diag "child LOOKUP count before=$parent_before after=$parent_after"
    tap_ok 1 "parent is resolved through the daemon's current namespace"
fi

node_status=$(/usr/sbin/usfs_testctl node-status 2>/dev/null)
parent_rebuilt=$(status_value "$node_status" parent_rebuilt)
if [ "$parent_rebuilt" -ge 1 ]; then
    tap_ok 0 "parent rebuild is recorded by vnode lifecycle accounting"
else
    tap_diag "node status after parent traversal: $node_status"
    tap_ok 1 "parent rebuild is recorded by vnode lifecycle accounting"
fi

holds=$(status_value "$node_status" holds)
releases=$(status_value "$node_status" releases)
if [ "$holds" -gt 0 ] && [ "$releases" -gt 0 ]; then
    tap_ok 0 "vnode reference acquisition and release paths are both exercised"
else
    tap_diag "node status before unmount: $node_status"
    tap_ok 1 "vnode reference acquisition and release paths are both exercised"
fi

stop_daemon
/usr/sbin/usfs_memfs --size=4 -f "$MOUNT" >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
usfs_track_pid "$DAEMON_PID"
replacement_failure=0
hardlink_failure=0
parent_failure=0
append_failure=0
if usfs_wait_until "replacement identity memfs mount" 30 is_mounted; then
    /usr/sbin/usfs_io_probe recreated-identity "$MOUNT" || replacement_failure=1
    /usr/sbin/usfs_io_probe hardlink-mappings "$MOUNT" || hardlink_failure=1
    /usr/sbin/usfs_io_probe moved-parent "$MOUNT" || parent_failure=1
    /usr/sbin/usfs_io_probe append-records "$MOUNT" || append_failure=1
else
    replacement_failure=1
    hardlink_failure=1
    parent_failure=1
    append_failure=1
fi
stop_daemon
tap_ok "$replacement_failure" "unlinked files and directories retain independent identities through replacement"
tap_ok "$hardlink_failure" "hard-link aliases share mapped pages, truncation, and retained object lifetime"
tap_ok "$parent_failure" "held working directories follow cross-parent moves without duplicate identities"
tap_ok "$append_failure" "multi-message append stays contiguous across aliases and detached handles"
node_status=$(/usr/sbin/usfs_testctl node-status 2>/dev/null)
created=$(status_value "$node_status" created)
reclaimed=$(status_value "$node_status" reclaimed)
live=$(status_value "$node_status" live)
holds=$(status_value "$node_status" holds)
releases=$(status_value "$node_status" releases)
root_created=$(status_value "$node_status" root_created)
root_reclaimed=$(status_value "$node_status" root_reclaimed)
duplicate_live=$(status_value "$node_status" duplicate_live)
accounting_errors=$(status_value "$node_status" accounting_errors)
if [ "$live" -eq 0 ] &&
   [ "$created" -eq "$reclaimed" ] &&
   [ $((created + holds)) -eq "$releases" ] &&
   [ "$root_created" -eq "$root_reclaimed" ] &&
   [ "$duplicate_live" -eq 0 ] &&
   [ "$accounting_errors" -eq 0 ]; then
    tap_ok 0 "final reclaim frees every vnode and balances every reference"
else
    tap_diag "final node status: $node_status"
    tap_ok 1 "final reclaim frees every vnode and balances every reference"
fi

tap_finish
