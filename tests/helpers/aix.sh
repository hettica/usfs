#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Shared TAP reporting, bounded waiting, coverage checkpoint, and owned
#   resource cleanup helpers for AIX-side USFS shell tests.
#
# Usage:
#   Source this library from a test in an AIX checkout:
#     USFS_SOURCE_DIR=/path/to/usfs
#     . "$USFS_SOURCE_DIR/tests/helpers/aix.sh"
#     tap_plan 1
#     tap_ok 0 "example assertion"
#     tap_finish
#
# Notes:
#   This file is a library, not a standalone test. Call usfs_track_pid and
#   usfs_track_mount for every resource the test creates, then install
#   usfs_owned_cleanup in a trap. Coverage checkpoints are no-ops unless
#   USFS_COVERAGE_SUITE names an active instrumented run.

# Tests are unattended even when their parent is an interactive SSH session.
# Utilities such as AIX rm otherwise read the inherited terminal for a
# confirmation while their prompt is commonly redirected into a test log.
exec </dev/null

USFS_TAP_PLANNED=0
USFS_TAP_EXECUTED=0
USFS_TAP_FAILED=0
USFS_TEST_PIDS=""
USFS_TEST_MOUNTS=""

usfs_coverage_checkpoint()
{
    # This preview does not include the coverage collection workflow.
    return 0
}

usfs_coverage_expect_export()
{
    # This preview does not include the coverage collection workflow.
    return 0
}

usfs_coverage_expect_profile()
{
    # This preview does not include the coverage collection workflow.
    return 0
}

tap_plan()
{
    USFS_TAP_PLANNED=$1
    echo "TAP version 13"
    echo "1..$1"
}

tap_diag()
{
    printf '# %s\n' "$*"
}

tap_ok()
{
    USFS_TAP_EXECUTED=$((USFS_TAP_EXECUTED + 1))
    if [ "$1" -eq 0 ]; then
        printf 'ok %d - %s\n' "$USFS_TAP_EXECUTED" "$2"
    else
        printf 'not ok %d - %s\n' "$USFS_TAP_EXECUTED" "$2"
        USFS_TAP_FAILED=$((USFS_TAP_FAILED + 1))
    fi
}

tap_is()
{
    if [ "$1" = "$2" ]; then
        tap_ok 0 "$3"
    else
        tap_diag "$3: actual='$1' expected='$2'"
        tap_ok 1 "$3"
    fi
}

tap_finish()
{
    if [ "$USFS_TAP_EXECUTED" -ne "$USFS_TAP_PLANNED" ]; then
        tap_diag "plan mismatch: planned=$USFS_TAP_PLANNED executed=$USFS_TAP_EXECUTED"
        return 2
    fi
    [ "$USFS_TAP_FAILED" -eq 0 ]
}

usfs_track_pid()
{
    USFS_TEST_PIDS="$USFS_TEST_PIDS $1"
}

usfs_track_mount()
{
    USFS_TEST_MOUNTS="$USFS_TEST_MOUNTS $1"
}

usfs_owned_cleanup()
{
    for mnt in $USFS_TEST_MOUNTS; do
        umount "$mnt" 2>/dev/null || umount -f "$mnt" 2>/dev/null
    done
    for pid in $USFS_TEST_PIDS; do
        kill -TERM "$pid" 2>/dev/null
    done
}

usfs_wait_until()
{
    description=$1
    timeout=$2
    shift 2
    deadline=$(( $(date +%s) + timeout ))
    next_heartbeat=$(( $(date +%s) + 15 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if "$@"; then
            return 0
        fi
        now=$(date +%s)
        if [ "$now" -ge "$next_heartbeat" ]; then
            tap_diag "heartbeat: waiting for $description"
            next_heartbeat=$((now + 15))
        fi
        sleep 1
    done
    tap_diag "timeout waiting for $description after ${timeout}s"
    return 1
}

# Pause a backend ancestor rename after mutation, then issue fstat through an
# already open child. The client's admission-wait hook identifies that caller
# before publication, even when an unrelated sync is also outstanding.
usfs_namespace_case()
(
    ns_base="/tmp/usfs-namespace.$$"
    ns_mount="$ns_base/mnt"
    ns_pids=""
    ns_mounted() { mount | grep -q " $ns_mount "; }
    ns_gone() { ! kill -0 "$1" 2>/dev/null; }
    ns_stat_waiting()
    {
        grep -q "^USFS_NAMESPACE_WAITING pid=$ns_holder exclusive=0$" "$ns_base/daemon.log"
    }
    ns_cleanup()
    {
        ns_cleanup_status=0
        # Release the callback before stopping its worker, including failures
        # before the normal publication step. A regular file never blocks here.
        if [ -d "$ns_base" ]; then
            : >"$ns_base/rename.release" || ns_cleanup_status=1
        fi

        for ns_pid in $ns_pids; do kill -TERM "$ns_pid" 2>/dev/null || true; done
        for ns_pid in $ns_pids; do
            if ! usfs_wait_until "namespace process cleanup" 5 ns_gone "$ns_pid"; then
                kill -KILL "$ns_pid" 2>/dev/null || true
                ns_cleanup_status=1
            fi
            wait "$ns_pid" 2>/dev/null || true
        done
        if ns_mounted; then
            umount "$ns_mount" || umount -f "$ns_mount" || ns_cleanup_status=1
        fi
        if ns_mounted; then ns_cleanup_status=1; else rm -rf "$ns_base"; fi
        return "$ns_cleanup_status"
    }
    trap 'ns_status=$?; trap - 0; if [ "$ns_status" -ne 0 ]; then cat "$ns_base"/*.log 2>/dev/null; fi; ns_cleanup || ns_status=1; exit "$ns_status"' 0
    trap 'exit 1' 1 2 15
    mkdir -p "$ns_mount" || exit 1
    mkfifo "$ns_base/stat" || exit 1
    USFS_MT_NAMESPACE_RELEASE="$ns_base/rename.release" /usr/sbin/usfs_mt_test_daemon \
        -f -o max_threads=4 "$ns_mount" >"$ns_base/daemon.log" 2>&1 &
    ns_daemon=$!; ns_pids="$ns_daemon"
    usfs_wait_until "namespace mount" 30 ns_mounted || exit 1
    /usr/sbin/usfs_io_probe stat-held "$ns_mount/old/file" "$ns_base/ready" \
        "$ns_base/stat" >"$ns_base/stat.log" 2>&1 &
    ns_holder=$!; ns_pids="$ns_pids $ns_holder"
    usfs_wait_until "held child descriptor" 15 test -f "$ns_base/ready" || exit 1
    /usr/sbin/usfs_io_probe rename "$ns_mount/old" "$ns_mount/new" >"$ns_base/rename.log" 2>&1 &
    ns_rename=$!; ns_pids="$ns_pids $ns_rename"
    usfs_wait_until "backend rename commit" 15 grep -q USFS_RENAME_COMMITTED "$ns_base/daemon.log" || exit 1

    # Reproduce the background-sync race deliberately. Its admission wait must
    # not be mistaken for the held stat, which is still stopped at its FIFO.
    sync &
    ns_sync=$!; ns_pids="$ns_pids $ns_sync"
    usfs_wait_until "concurrent sync admission" 10 grep -q '^USFS_NAMESPACE_WAITING .* exclusive=0$' "$ns_base/daemon.log" || exit 1

    printf x >"$ns_base/stat" &
    ns_command=$!; ns_pids="$ns_pids $ns_command"
    usfs_wait_until "held stat command" 10 ns_gone "$ns_command" || exit 1
    wait "$ns_command" || exit 1
    usfs_wait_until "rename and held stat overlap" 10 ns_stat_waiting || exit 1
    : >"$ns_base/rename.release" || exit 1
    usfs_wait_until "ancestor rename completion" 15 ns_gone "$ns_rename" || exit 1
    wait "$ns_rename" || exit 1
    usfs_wait_until "held child stat completion" 15 ns_gone "$ns_holder" || exit 1
    wait "$ns_holder" || exit 1
    usfs_wait_until "concurrent sync completion" 15 ns_gone "$ns_sync" || exit 1
    wait "$ns_sync" || exit 1
    grep -q '^held-stat=valid$' "$ns_base/stat.log" || exit 1
    grep -q '^NAMESPACE_GETATTR /new/file valid=1$' "$ns_base/daemon.log" || exit 1
    kill -TERM "$ns_daemon" || exit 1
    usfs_wait_until "namespace daemon exit" 15 ns_gone "$ns_daemon" || exit 1
    wait "$ns_daemon" || exit 1
    ! ns_mounted
)

# Run in a subshell so nested ownership bookkeeping cannot disturb the caller.
# The old daemon pauses on a FIFO after external removal, making replacement
# publication complete before its library cleanup resumes.
usfs_mount_owner_case()
(
    owner_mode=$1
    owner_layers=$2
    owner_base="/tmp/usfs-mount-owner.$$.${owner_mode}.${owner_layers}"
    owner_mount="$owner_base/mnt"
    owner_pids=""
    owner_held_fd=0
    owner_holder=0
    owner_count()
    {
        mount | awk -v path="$owner_mount" '{ for (i=1;i<=NF;i++) if ($i==path) { n++; break } } END { print n+0 }'
    }
    owner_has_count() { [ "$(owner_count)" -eq "$1" ]; }
    owner_gone() { ! kill -0 "$1" 2>/dev/null; }
    owner_cleanup()
    {
        owner_cleanup_status=0
        if [ "$owner_held_fd" -eq 1 ]; then
            exec 3<&- || owner_cleanup_status=1
        fi
        if [ "$owner_holder" -ne 0 ]; then
            kill -TERM "$owner_holder" 2>/dev/null || true
            wait "$owner_holder" 2>/dev/null || true
        fi
        for owner_attempt in 1 2 3; do
            owner_has_count 0 && break
            umount "$owner_mount" >/dev/null 2>&1 ||
                umount -f "$owner_mount" >/dev/null 2>&1 || owner_cleanup_status=1
        done
        for owner_pid in $owner_pids; do
            kill -TERM "$owner_pid" 2>/dev/null || true
            wait "$owner_pid" 2>/dev/null || true
        done
        if owner_has_count 0; then
            rm -rf "$owner_base"
        else
            tap_diag "mount-owner cleanup left a mount at $owner_mount"
            owner_cleanup_status=1
        fi
        return "$owner_cleanup_status"
    }
    trap 'owner_status=$?; trap - 0; owner_cleanup || owner_status=1; exit "$owner_status"' 0
    trap 'exit 1' 1 2 15
    mkdir -p "$owner_mount" || exit 1
    mkfifo "$owner_base/release" || exit 1
    case "$owner_mode" in
    retry-*)
        USFS_MT_RETRY_FIFO="$owner_base/release" \
        /usr/sbin/usfs_mt_test_daemon -f "$owner_mount" >"$owner_base/old.log" 2>&1 &
        ;;
    *)
        USFS_MT_CLEANUP_FIFO="$owner_base/release" \
        /usr/sbin/usfs_mt_test_daemon -f "$owner_mount" >"$owner_base/old.log" 2>&1 &
        ;;
    esac
    owner_old=$!
    owner_pids="$owner_old"
    usfs_wait_until "original mount owner" 30 owner_has_count 1 || exit 1
    usfs_wait_until "original daemon signal readiness" 15 grep -q USFS_OWNER_READY "$owner_base/old.log" || exit 1
    case "$owner_mode" in
    retry-*)
        if [ "$owner_mode" = retry-descriptor ]; then
            exec 3<"$owner_mount/slow" || exit 1
            owner_held_fd=1
        else
            /usr/sbin/usfs_io_probe mmap-hold "$owner_mount/slow" 4096 0 \
                "$owner_base/map.ready" "$owner_base/map.release" >"$owner_base/map.log" 2>&1 &
            owner_holder=$!
            owner_pids="$owner_pids $owner_holder"
            usfs_wait_until "held mapping readiness" 15 test -f "$owner_base/map.ready" || exit 1
        fi
        printf x >"$owner_base/release" &
        owner_writer=$!
        owner_pids="$owner_pids $owner_writer"
        usfs_wait_until "first unmount command" 10 owner_gone "$owner_writer" || exit 1
        wait "$owner_writer" || exit 1
        usfs_wait_until "failed unmount and refused destruction" 15 grep -q USFS_RETRY_READY "$owner_base/old.log" || {
            cat "$owner_base/old.log"
            exit 1
        }
        owner_has_count 1 || exit 1
        if [ "$owner_held_fd" -eq 1 ]; then
            exec 3<&- || exit 1
            owner_held_fd=0
        else
            : >"$owner_base/map.release"
            usfs_wait_until "final mapping release" 15 owner_gone "$owner_holder" || exit 1
            wait "$owner_holder" || exit 1
            owner_holder=0
            grep -q 'result=0 stage=second' "$owner_base/map.log" || exit 1
        fi
        printf x >"$owner_base/release" &
        owner_writer=$!
        owner_pids="$owner_pids $owner_writer"
        usfs_wait_until "retry command delivery" 10 owner_gone "$owner_writer" || exit 1
        wait "$owner_writer" || exit 1
        usfs_wait_until "successful unmount retry" 15 owner_gone "$owner_old" || exit 1
        wait "$owner_old" || exit 1
        owner_has_count 0 && grep -q USFS_RETRY_DONE "$owner_base/old.log"
        exit $?
        ;;
    esac
    if [ "$owner_mode" = forced ]; then
        umount -f "$owner_mount" || exit 1
    else
        umount "$owner_mount" || exit 1
    fi
    usfs_wait_until "old daemon cleanup barrier" 15 grep -q USFS_CLEANUP_READY "$owner_base/old.log" || exit 1
    owner_layer=1
    while [ "$owner_layer" -le "$owner_layers" ]; do
        /usr/sbin/usfs_mt_test_daemon -f "$owner_mount" >"$owner_base/new.$owner_layer.log" 2>&1 &
        owner_pids="$owner_pids $!"
        usfs_wait_until "replacement layer $owner_layer" 30 owner_has_count "$owner_layer" || exit 1
        owner_layer=$((owner_layer + 1))
    done
    printf x >"$owner_base/release" &
    owner_writer=$!
    owner_pids="$owner_pids $owner_writer"
    usfs_wait_until "old mount cleanup completion" 15 owner_gone "$owner_old" || exit 1
    wait "$owner_old" || exit 1
    usfs_wait_until "cleanup barrier writer" 10 owner_gone "$owner_writer" || exit 1
    wait "$owner_writer" || exit 1
    owner_has_count "$owner_layers" || {
        tap_diag "old cleanup removed a replacement mount"
        cat "$owner_base/old.log"
        exit 1
    }
    [ "$(cat "$owner_mount/slow")" = parallel-dispatch ] || exit 1
)
