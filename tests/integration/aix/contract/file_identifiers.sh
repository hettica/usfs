#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Exercise native AIX fidtovp and vnode release against mount-scoped backend
#   identities, including namespace changes and daemon/reload boundaries.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Requires the testing kernel and its usfs_fid_probe helper.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-file-identifiers.$$
MOUNT=$BASE/mnt
PROBE=/usr/sbin/usfs_fid_probe
DAEMON_PID=0
CASE_PID=0
COMMAND_PID=0
LOOKUP_PID=0
RESOLVE_PID=0
CAPTURE_PID=0
HELD_FILE_OPEN=0
CASE_TIMEOUT_SECONDS=30
PROCESS_GRACE_SECONDS=5
LOOP_ITERATIONS=12

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

process_finished()
{
    ! kill -0 "$1" 2>/dev/null
}

stop_owned_process()
{
    local description=$1
    local process_id=$2

    if process_finished "$process_id"; then
        wait "$process_id" 2>/dev/null || true
        return 0
    fi

    kill -TERM "$process_id" 2>/dev/null || true
    if ! usfs_wait_until "$description termination" "$PROCESS_GRACE_SECONDS" process_finished "$process_id"; then
        kill -KILL "$process_id" 2>/dev/null || true
        if ! usfs_wait_until "$description kill" "$PROCESS_GRACE_SECONDS" process_finished "$process_id"; then
            tap_diag "$description remains active after SIGKILL: pid=$process_id"
            return 1
        fi
    fi

    wait "$process_id" 2>/dev/null || true
    return 0
}

wait_owned_process()
{
    local description=$1
    local process_id=$2
    local timeout_seconds=$3

    if usfs_wait_until "$description" "$timeout_seconds" process_finished "$process_id"; then
        wait "$process_id"
        return $?
    fi

    stop_owned_process "$description" "$process_id" || true
    return 1
}

run_bounded_command()
{
    local description=$1
    local timeout_seconds=$2
    shift 2

    if [ "$COMMAND_PID" -ne 0 ]; then
        tap_diag "cannot start $description while an earlier command is active: pid=$COMMAND_PID"
        return 1
    fi

    "$@" &
    COMMAND_PID=$!
    if wait_owned_process "$description" "$COMMAND_PID" "$timeout_seconds"; then
        COMMAND_PID=0
        return 0
    fi

    if process_finished "$COMMAND_PID"; then COMMAND_PID=0; fi
    return 1
}

bounded_unmount()
{
    local description=$1
    shift
    run_bounded_command "$description" "$CASE_TIMEOUT_SECONDS" umount "$@"
}

stop_backend()
{
    local recovery_mode=${1:-clean}
    local stop_failed=0

    if is_mounted; then
        if ! bounded_unmount "FID clean unmount" "$MOUNT" >/dev/null 2>&1; then
            if [ "$recovery_mode" != daemon-loss ]; then stop_failed=1; fi
            if [ "$COMMAND_PID" -ne 0 ] ||
               ! bounded_unmount "FID forced cleanup unmount" -f "$MOUNT" >/dev/null 2>&1; then
                stop_failed=1
            fi
        fi
    fi

    if [ "$DAEMON_PID" -ne 0 ]; then
        stop_owned_process "FID daemon" "$DAEMON_PID" || stop_failed=1
        if process_finished "$DAEMON_PID"; then DAEMON_PID=0; fi
    fi

    is_mounted && stop_failed=1
    return "$stop_failed"
}

restore_device()
{
    local device_status
    device_status=$(lsdev -l usfs0 -F status 2>/dev/null) || return 1

    case "$device_status" in
        Available) return 0 ;;
        Defined) run_bounded_command "FID device restoration" "$CASE_TIMEOUT_SECONDS" mkdev -l usfs0 ;;
        *) return 1 ;;
    esac
}

cleanup()
{
    local cleanup_failed=0
    if [ "$CASE_PID" -ne 0 ]; then
        stop_owned_process "FID probe" "$CASE_PID" || cleanup_failed=1
        if process_finished "$CASE_PID"; then CASE_PID=0; fi
    fi
    if [ "$LOOKUP_PID" -ne 0 ]; then
        stop_owned_process "FID lookup probe" "$LOOKUP_PID" || cleanup_failed=1
        if process_finished "$LOOKUP_PID"; then LOOKUP_PID=0; fi
    fi
    if [ "$RESOLVE_PID" -ne 0 ]; then
        stop_owned_process "FID resolve probe" "$RESOLVE_PID" || cleanup_failed=1
        if process_finished "$RESOLVE_PID"; then RESOLVE_PID=0; fi
    fi
    if [ "$CAPTURE_PID" -ne 0 ]; then
        stop_owned_process "FID capture probe" "$CAPTURE_PID" || cleanup_failed=1
        if process_finished "$CAPTURE_PID"; then CAPTURE_PID=0; fi
    fi
    if [ "$COMMAND_PID" -ne 0 ]; then
        stop_owned_process "FID command" "$COMMAND_PID" || cleanup_failed=1
    fi
    if [ "$COMMAND_PID" -ne 0 ] && process_finished "$COMMAND_PID"; then COMMAND_PID=0; fi

    if [ "$HELD_FILE_OPEN" -eq 1 ]; then exec 3<&-; fi
    stop_backend || cleanup_failed=1
    restore_device || cleanup_failed=1
    if ! is_mounted && [ "$DAEMON_PID" -eq 0 ] && [ "$CASE_PID" -eq 0 ] &&
       [ "$LOOKUP_PID" -eq 0 ] && [ "$RESOLVE_PID" -eq 0 ] &&
       [ "$CAPTURE_PID" -eq 0 ] && [ "$COMMAND_PID" -eq 0 ]; then
        rm -rf "$BASE"
    fi
    return "$cleanup_failed"
}

trap 'result=$?; trap - 0; cleanup || result=1; exit "$result"' 0
trap 'exit 1' 1 2 15
mkdir -p "$MOUNT" "$BASE/backing" || exit 1

start_backend()
{
    "$@" -f -o max_threads=4 "$MOUNT" >"$BASE/daemon.log" 2>&1 &
    DAEMON_PID=$!
    if ! usfs_wait_until "FID backend mount" 30 is_mounted; then
        cat "$BASE/daemon.log"
        return 1
    fi
}

run_case()
{
    local description=$1
    shift

    run_probe_status "$description" "$@"
    local case_failed=$?
    tap_ok "$case_failed" "$description"

    if [ "$CASE_PID" -ne 0 ]; then exit 1; fi
}

run_probe_status()
{
    local description=$1
    local case_failed=0
    shift

    "$@" >"$BASE/case.log" 2>&1 &
    CASE_PID=$!
    wait_owned_process "$description" "$CASE_PID" "$CASE_TIMEOUT_SECONDS" || case_failed=1
    if process_finished "$CASE_PID"; then CASE_PID=0; fi

    if [ "$case_failed" -ne 0 ]; then sed 's/^/# /' "$BASE/case.log"; fi
    return "$case_failed"
}

read_node_counts()
{
    local status_line
    local field

    run_bounded_command "native FID node status" 10 /usr/sbin/usfs_testctl node-status >"$BASE/node-status.log" 2>&1 || return 1
    IFS= read -r status_line <"$BASE/node-status.log" || return 1

    NODE_CREATED=
    NODE_RECLAIMED=
    NODE_LIVE=
    for field in $status_line; do
        case "$field" in
            created=*) NODE_CREATED=${field#created=} ;;
            reclaimed=*) NODE_RECLAIMED=${field#reclaimed=} ;;
            live=*) NODE_LIVE=${field#live=} ;;
        esac
    done

    case "$NODE_CREATED:$NODE_RECLAIMED:$NODE_LIVE" in
        *[!0-9:]* | :* | *::* | *:) return 1 ;;
    esac

    return 0
}

verify_eviction_progress()
{
    local previous_created=$1
    local previous_reclaimed=$2

    if [ "$NODE_LIVE" -ne 0 ] || [ "$NODE_CREATED" -le "$previous_created" ] ||
       [ "$NODE_RECLAIMED" -le "$previous_reclaimed" ] ||
       [ $((NODE_CREATED - previous_created)) -ne $((NODE_RECLAIMED - previous_reclaimed)) ]; then
        tap_diag "native FID eviction counters: created=$NODE_CREATED reclaimed=$NODE_RECLAIMED live=$NODE_LIVE; previous_created=$previous_created previous_reclaimed=$previous_reclaimed"
        return 1
    fi

    return 0
}

verify_native_eviction()
{
    if ! read_node_counts; then
        tap_diag "failed to read initial native FID node status"
        return 1
    fi

    if [ "$NODE_LIVE" -ne 0 ]; then
        tap_diag "empty-file baseline retained $NODE_LIVE live vnodes; expected zero: $(cat "$BASE/node-status.log")"
        return 1
    fi

    local previous_created=$NODE_CREATED
    local previous_reclaimed=$NODE_RECLAIMED

    if ! run_probe_status "native FID capture after vnode eviction" "$PROBE" capture "$MOUNT/eviction" "$BASE/eviction.fid"; then
        return 1
    fi

    if ! read_node_counts || ! verify_eviction_progress "$previous_created" "$previous_reclaimed"; then
        return 1
    fi

    previous_created=$NODE_CREATED
    previous_reclaimed=$NODE_RECLAIMED

    if ! run_probe_status "native FID resolve after vnode eviction" "$PROBE" resolve "$MOUNT" "$BASE/eviction.fid" "$MOUNT/eviction"; then
        return 1
    fi

    read_node_counts && verify_eviction_progress "$previous_created" "$previous_reclaimed"
}

native_eviction_case()
{
    local case_failed=0

    run_bounded_command "create empty FID eviction file" 10 touch "$MOUNT/eviction" >"$BASE/eviction-create.log" 2>&1 || case_failed=1
    if [ "$case_failed" -eq 0 ]; then verify_native_eviction || case_failed=1; fi

    if [ "$CASE_PID" -ne 0 ] || [ "$COMMAND_PID" -ne 0 ]; then
        tap_ok 1 "native FID reconstructs an evicted vnode on capture and resolve"
        exit 1
    fi

    run_bounded_command "remove FID eviction file" 10 rm -f "$MOUNT/eviction" >"$BASE/eviction-remove.log" 2>&1 || case_failed=1
    if [ "$case_failed" -ne 0 ]; then sed 's/^/# /' "$BASE/node-status.log" "$BASE/eviction-create.log" "$BASE/eviction-remove.log" 2>/dev/null; fi
    tap_ok "$case_failed" "native FID reconstructs an evicted vnode on capture and resolve"

    if [ "$COMMAND_PID" -ne 0 ]; then exit 1; fi
}

parallel_lookup_and_vget()
{
    local parallel_failed=0

    "$PROBE" resolve-repeat "$MOUNT" "$BASE/file.fid" "$MOUNT/renamed/file" "$LOOP_ITERATIONS" >"$BASE/lookup.log" 2>&1 &
    LOOKUP_PID=$!
    "$PROBE" resolve-repeat "$MOUNT" "$BASE/file.fid" "$MOUNT/renamed/file" "$LOOP_ITERATIONS" >"$BASE/resolve.log" 2>&1 &
    RESOLVE_PID=$!

    wait_owned_process "parallel pathname lookup" "$LOOKUP_PID" "$CASE_TIMEOUT_SECONDS" || parallel_failed=1
    if process_finished "$LOOKUP_PID"; then LOOKUP_PID=0; fi
    wait_owned_process "parallel native vget" "$RESOLVE_PID" "$CASE_TIMEOUT_SECONDS" || parallel_failed=1
    if process_finished "$RESOLVE_PID"; then RESOLVE_PID=0; fi

    if [ "$parallel_failed" -ne 0 ]; then
        sed 's/^/# /' "$BASE/lookup.log" "$BASE/resolve.log"
    fi
    tap_ok "$parallel_failed" "parallel pathname lookup and vget share canonical identity"

    if [ "$LOOKUP_PID" -ne 0 ] || [ "$RESOLVE_PID" -ne 0 ]; then exit 1; fi
}

vget_reserved()
{
    local schedule_status
    schedule_status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null) || return 1
    case "$schedule_status" in
        *'schedule=6 reached=0x2000 '*) return 0 ;;
        *) return 1 ;;
    esac
}

concurrent_forced_unmount()
{
    local race_failed=0
    local schedule_status

    "$PROBE" capture-stale "$MOUNT/file" >"$BASE/race.log" 2>&1 &
    CAPTURE_PID=$!

    usfs_wait_until "native VGET reservation" 10 vget_reserved || race_failed=1
    bounded_unmount "forced unmount during native vget" -f "$MOUNT" >"$BASE/race-umount.log" 2>&1 || race_failed=1
    wait_owned_process "native VGET race drain" "$CAPTURE_PID" "$CASE_TIMEOUT_SECONDS" || race_failed=1
    if process_finished "$CAPTURE_PID"; then CAPTURE_PID=0; fi

    schedule_status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null) || race_failed=1
    case "$schedule_status" in
        *'schedule=6 reached=0x6000 '*'timed_out=0 active=0'*) ;;
        *) race_failed=1 ;;
    esac

    if [ "$race_failed" -ne 0 ]; then
        sed 's/^/# /' "$BASE/race.log" "$BASE/race-umount.log" 2>/dev/null
        tap_diag "FID race schedule status: $schedule_status"
    fi
    tap_ok "$race_failed" "concurrent native VGET and forced unmount drain without publication after detach"

    if [ "$CAPTURE_PID" -ne 0 ] || [ "$COMMAND_PID" -ne 0 ]; then exit 1; fi
}

tap_plan 23
start_backend /usr/sbin/usfs_memfs || exit 1
native_eviction_case
stop_backend || exit 1

start_backend /usr/sbin/usfs_memfs || exit 1
mkdir "$MOUNT/original" || exit 1
printf 'fid-data\n' >"$MOUNT/original/file" || exit 1
ln -s file "$MOUNT/original/symlink" || exit 1

run_case "root fid passes native fidtovp and release" "$PROBE" capture "$MOUNT" "$BASE/root.fid"
run_case "file fid passes native fidtovp and release" "$PROBE" capture "$MOUNT/original/file" "$BASE/file.fid"
run_case "directory fid passes native fidtovp and release" "$PROBE" capture "$MOUNT/original" "$BASE/directory.fid"
run_case "symlink fid preserves the link vnode" "$PROBE" capture "$MOUNT/original/symlink" "$BASE/symlink.fid"
run_case "saved root fid returns the canonical vnode" "$PROBE" resolve "$MOUNT" "$BASE/root.fid" "$MOUNT"
run_case "saved file fid survives release between commands" "$PROBE" resolve "$MOUNT" "$BASE/file.fid" "$MOUNT/original/file"
run_case "malformed identifiers are rejected by native vget" "$PROBE" malformed "$MOUNT" "$BASE/file.fid"

mv "$MOUNT/original" "$MOUNT/renamed" || exit 1
run_case "directory token survives its rename" "$PROBE" resolve "$MOUNT" "$BASE/directory.fid" "$MOUNT/renamed"
run_case "file token survives ancestor rename" "$PROBE" resolve "$MOUNT" "$BASE/file.fid" "$MOUNT/renamed/file"
run_case "symlink token survives ancestor rename" "$PROBE" resolve "$MOUNT" "$BASE/symlink.fid" "$MOUNT/renamed/symlink"
parallel_lookup_and_vget

ln "$MOUNT/renamed/file" "$MOUNT/survivor" || exit 1
rm "$MOUNT/renamed/file" || exit 1
run_case "token resolves a surviving hard link after original alias removal" "$PROBE" resolve "$MOUNT" "$BASE/file.fid" "$MOUNT/survivor"
exec 3<"$MOUNT/survivor" || exit 1
HELD_FILE_OPEN=1
rm "$MOUNT/survivor" || exit 1
run_case "final unlink makes a held-open object's token stale" "$PROBE" stale "$MOUNT" "$BASE/file.fid"
printf 'replacement\n' >"$MOUNT/survivor" || exit 1
run_case "replacement does not revive the old token" "$PROBE" stale "$MOUNT" "$BASE/file.fid"
exec 3<&-
HELD_FILE_OPEN=0
run_case "replacement exports its own valid identity" "$PROBE" capture "$MOUNT/survivor" "$BASE/replacement.fid"

stop_backend || exit 1
start_backend /usr/sbin/usfs_memfs || exit 1
run_case "saved root fid is stale after remount" "$PROBE" stale "$MOUNT" "$BASE/root.fid"
printf 'daemon-loss\n' >"$MOUNT/file" || exit 1
run_case "fresh mount exports an independent file identity" "$PROBE" capture "$MOUNT/file" "$BASE/loss.fid"
kill -KILL "$DAEMON_PID" || exit 1
usfs_wait_until "killed FID daemon" 10 process_finished "$DAEMON_PID" || exit 1
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=0
run_case "confirmed daemon loss makes a saved identity stale" "$PROBE" stale "$MOUNT" "$BASE/loss.fid"
stop_backend daemon-loss || exit 1

schedule_failure=0
run_bounded_command "native VGET race schedule arm" 10 /usr/sbin/usfs_testctl schedule 6 >"$BASE/race-arm.log" 2>&1 || schedule_failure=1
if [ "$schedule_failure" -ne 0 ]; then sed 's/^/# /' "$BASE/race-arm.log"; fi
tap_ok "$schedule_failure" "native VGET race schedule arms before mounting"
if [ "$schedule_failure" -ne 0 ]; then exit 1; fi

start_backend /usr/sbin/usfs_memfs || exit 1
printf 'race-data\n' >"$MOUNT/file" || exit 1
concurrent_forced_unmount
stop_backend || exit 1

printf 'mirror-data\n' >"$BASE/backing/file" || exit 1
start_backend /usr/sbin/usfs_mirror "--source=$BASE/backing" || exit 1
run_case "mirror refuses identifiers without identity callbacks" "$PROBE" unsupported "$MOUNT/file"
stop_backend || exit 1

reload_result=0
run_bounded_command "FID device removal" "$CASE_TIMEOUT_SECONDS" rmdev -l usfs0 >"$BASE/reload.log" 2>&1 || reload_result=1
if [ "$reload_result" -eq 0 ]; then
    run_bounded_command "FID device reconfiguration" "$CASE_TIMEOUT_SECONDS" mkdev -l usfs0 >>"$BASE/reload.log" 2>&1 || reload_result=1
fi
if [ "$reload_result" -ne 0 ]; then cat "$BASE/reload.log"; fi
tap_ok "$reload_result" "all native FID references drain before clean unload and reload"
tap_finish
