#!/usr/bin/bash
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose:
#   Verify mount/channel allocation races against CFG_TERM, including
#   admission-gate closure, busy rollback, drain, and clean reconfiguration.
#
# Usage:
#   Run through `make test` in the AIX CMake binary directory.
#   Directly on a testing AIX deployment:
#     cd /home && bash tests/integration/aix/concurrency/lifecycle.sh
#
# Notes:
#   Requires USFS_TESTING schedule controls. It may temporarily transition
#   usfs0 between Available and Defined, but restores a usable quiescent device.

set -u

. "${USFS_SOURCE_DIR:?}/tests/helpers/aix.sh"

BASE=/tmp/usfs-lifecycle-races.$$
MOUNT=$BASE/mnt
SCENARIO=$BASE/scenario
REQUESTS=$BASE/requests.log
DAEMON_LOG=$BASE/daemon.log
DAEMON_PID=0
COMMAND_PID=0
MOUNT_TERM_PID=0
CHANNEL_ONE_PID=0
CHANNEL_TWO_PID=0
CHANNEL_TERM_PID=0
DEALLOCATION_ARM_PID=0
DEALLOCATION_TERM_PID=0
STATUS_FD_THREE_OPEN=0
STATUS_FD_FOUR_OPEN=0
COMMAND_TIMEOUT_SECONDS=30
PROCESS_GRACE_SECONDS=5
PROCESS_WAIT_TIMEOUT=124

is_mounted()
{
    mount 2>/dev/null | grep -q " $MOUNT "
}

device_status()
{
    lsdev -l usfs0 -F status 2>/dev/null
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
    return "$PROCESS_WAIT_TIMEOUT"
}

run_bounded_command()
{
    local description=$1
    local timeout_seconds=$2
    shift 2

    if [ "$COMMAND_PID" -ne 0 ]; then
        tap_diag "cannot start $description while an earlier command is active: pid=$COMMAND_PID"
        return "$PROCESS_WAIT_TIMEOUT"
    fi

    "$@" &
    COMMAND_PID=$!
    wait_owned_process "$description" "$COMMAND_PID" "$timeout_seconds"
    local command_rc=$?

    if process_finished "$COMMAND_PID"; then COMMAND_PID=0; fi
    return "$command_rc"
}

bounded_unmount()
{
    local description=$1
    shift
    run_bounded_command "$description" "$COMMAND_TIMEOUT_SECONDS" umount "$@"
}

restore_device()
{
    local status
    status=$(device_status)
    case "$status" in
        Available) return 0 ;;
        Defined) run_bounded_command "lifecycle device restoration" "$COMMAND_TIMEOUT_SECONDS" mkdev -l usfs0 >/dev/null 2>&1 ;;
        *) return 1 ;;
    esac
}

stop_daemon()
{
    local stop_failed=0

    if is_mounted; then
        if ! bounded_unmount "lifecycle clean unmount" "$MOUNT" >/dev/null 2>&1; then
            stop_failed=1
            if [ "$COMMAND_PID" -eq 0 ]; then
                bounded_unmount "lifecycle forced cleanup unmount" -f "$MOUNT" >/dev/null 2>&1 || stop_failed=1
            fi
        fi
    fi

    if [ "$DAEMON_PID" -ne 0 ]; then
        stop_owned_process "lifecycle daemon" "$DAEMON_PID" || stop_failed=1
        if process_finished "$DAEMON_PID"; then DAEMON_PID=0; fi
    fi

    is_mounted && stop_failed=1
    return "$stop_failed"
}

cleanup()
{
    local cleanup_failed=0

    if [ "$STATUS_FD_THREE_OPEN" -eq 1 ]; then exec 3<&-; fi
    if [ "$STATUS_FD_FOUR_OPEN" -eq 1 ]; then exec 4<&-; fi

    for process_id in "$MOUNT_TERM_PID" "$CHANNEL_ONE_PID" "$CHANNEL_TWO_PID" "$CHANNEL_TERM_PID" "$DEALLOCATION_ARM_PID" "$DEALLOCATION_TERM_PID" "$COMMAND_PID"; do
        if [ "$process_id" -ne 0 ]; then
            stop_owned_process "lifecycle owned process" "$process_id" || cleanup_failed=1
        fi
    done
    if [ "$MOUNT_TERM_PID" -ne 0 ] && process_finished "$MOUNT_TERM_PID"; then MOUNT_TERM_PID=0; fi
    if [ "$CHANNEL_ONE_PID" -ne 0 ] && process_finished "$CHANNEL_ONE_PID"; then CHANNEL_ONE_PID=0; fi
    if [ "$CHANNEL_TWO_PID" -ne 0 ] && process_finished "$CHANNEL_TWO_PID"; then CHANNEL_TWO_PID=0; fi
    if [ "$CHANNEL_TERM_PID" -ne 0 ] && process_finished "$CHANNEL_TERM_PID"; then CHANNEL_TERM_PID=0; fi
    if [ "$DEALLOCATION_ARM_PID" -ne 0 ] && process_finished "$DEALLOCATION_ARM_PID"; then DEALLOCATION_ARM_PID=0; fi
    if [ "$DEALLOCATION_TERM_PID" -ne 0 ] && process_finished "$DEALLOCATION_TERM_PID"; then DEALLOCATION_TERM_PID=0; fi
    if [ "$COMMAND_PID" -ne 0 ] && process_finished "$COMMAND_PID"; then COMMAND_PID=0; fi

    stop_daemon || cleanup_failed=1
    restore_device || cleanup_failed=1
    if ! is_mounted && [ "$DAEMON_PID" -eq 0 ] && [ "$COMMAND_PID" -eq 0 ] &&
       [ "$MOUNT_TERM_PID" -eq 0 ] && [ "$CHANNEL_ONE_PID" -eq 0 ] &&
       [ "$CHANNEL_TWO_PID" -eq 0 ] && [ "$CHANNEL_TERM_PID" -eq 0 ] &&
       [ "$DEALLOCATION_ARM_PID" -eq 0 ] && [ "$DEALLOCATION_TERM_PID" -eq 0 ]; then
        rm -rf "$BASE"
    fi

    return "$cleanup_failed"
}

no_usfs_state()
{
    mounts=$(mount 2>/dev/null |
        awk '$3 == "usfs" { n++ } END { print n+0 }')
    daemons=$(ps -ef | grep -c '[u]sfs_scenario_daemon')
    users=$(fuser /dev/usfs0 2>/dev/null | wc -w | tr -d ' ')
    [ "$mounts" -eq 0 ] && [ "$daemons" -eq 0 ] && [ "$users" -eq 0 ]
}

trap 'result=$?; trap - 0; cleanup || result=1; exit "$result"' 0
trap 'exit 1' 1 2 15
mkdir -p "$MOUNT"
printf 'USFS-SCENARIO 1\n' >"$SCENARIO"
tap_plan 19

mount_failure=0
run_bounded_command "mount schedule arm" "$COMMAND_TIMEOUT_SECONDS" /usr/sbin/usfs_testctl schedule 1 >/dev/null 2>&1 ||
    mount_failure=1
tap_ok "$mount_failure" "mount-versus-termination schedule arms while quiescent"

/usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
    >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
rmdev -l usfs0 >"$BASE/mount-rmdev.log" 2>&1 &
MOUNT_TERM_PID=$!

if usfs_wait_until "scheduled mount" 20 is_mounted; then
    tap_ok 0 "mount reservation completes through the termination rendezvous"
else
    tap_diag "daemon log follows"
    sed 's/^/# /' "$DAEMON_LOG" 2>/dev/null
    tap_ok 1 "mount reservation completes through the termination rendezvous"
fi

wait_owned_process "mount-race termination" "$MOUNT_TERM_PID" "$COMMAND_TIMEOUT_SECONDS"
MOUNT_TERM_RC=$?
if process_finished "$MOUNT_TERM_PID"; then MOUNT_TERM_PID=0; else exit 1; fi
if [ "$MOUNT_TERM_RC" -ne 0 ] && [ "$MOUNT_TERM_RC" -ne "$PROCESS_WAIT_TIMEOUT" ] &&
   [ "$(device_status)" = "Available" ]; then
    tap_ok 0 "CFG_TERM observes the reserved mount and fails busy"
else
    sed 's/^/# /' "$BASE/mount-rmdev.log" 2>/dev/null
    tap_ok 1 "CFG_TERM observes the reserved mount and fails busy"
fi

schedule_status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null)
case "$schedule_status" in
    *'schedule=1 reached=0x7 '*'timed_out=0 active=0'*)
        tap_ok 0 "mount schedule reached reserve, gate-close, and reopen points" ;;
    *)
        tap_diag "schedule status: $schedule_status"
        tap_ok 1 "mount schedule reached reserve, gate-close, and reopen points" ;;
esac

if /usr/sbin/usfs_testctl info >/dev/null 2>&1; then
    tap_ok 0 "busy termination reopens admission for a new channel"
else
    tap_ok 1 "busy termination reopens admission for a new channel"
fi

stop_daemon || mount_failure=1
usfs_coverage_checkpoint lifecycle-pre-drained-term ||
    mount_failure=1
usfs_coverage_expect_export lifecycle-drained-term ||
    mount_failure=1
USFS_COVERAGE_EVENT=lifecycle-drained-term \
    run_bounded_command "drained mount termination" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 >"$BASE/post-mount-rmdev.log" 2>&1 || mount_failure=1
restore_device || mount_failure=1
if [ "$mount_failure" -ne 0 ]; then
    sed 's/^/# /' "$BASE/post-mount-rmdev.log" 2>/dev/null
fi
tap_ok "$mount_failure" "drained mount permits termination and clean reconfiguration"

channel_failure=0
usfs_coverage_checkpoint lifecycle-pre-channel-term ||
    channel_failure=1
# Busy rollback retains the live generation; the successful retry exports it.
run_bounded_command "channel schedule arm" "$COMMAND_TIMEOUT_SECONDS" /usr/sbin/usfs_testctl schedule 2 >/dev/null 2>&1 ||
    channel_failure=1

/usr/sbin/usfs_testctl info >"$BASE/channel-one.log" 2>&1 &
CHANNEL_ONE_PID=$!
/usr/sbin/usfs_testctl info >"$BASE/channel-two.log" 2>&1 &
CHANNEL_TWO_PID=$!
USFS_COVERAGE_EVENT=lifecycle-channel-term \
    rmdev -l usfs0 >"$BASE/channel-rmdev.log" 2>&1 &
CHANNEL_TERM_PID=$!

wait_owned_process "first channel callback" "$CHANNEL_ONE_PID" "$COMMAND_TIMEOUT_SECONDS"; CHANNEL_ONE_RC=$?
if process_finished "$CHANNEL_ONE_PID"; then CHANNEL_ONE_PID=0; else exit 1; fi
wait_owned_process "second channel callback" "$CHANNEL_TWO_PID" "$COMMAND_TIMEOUT_SECONDS"; CHANNEL_TWO_RC=$?
if process_finished "$CHANNEL_TWO_PID"; then CHANNEL_TWO_PID=0; else exit 1; fi
wait_owned_process "channel-race termination" "$CHANNEL_TERM_PID" "$COMMAND_TIMEOUT_SECONDS"; CHANNEL_TERM_RC=$?
if process_finished "$CHANNEL_TERM_PID"; then CHANNEL_TERM_PID=0; else exit 1; fi

if [ "$channel_failure" -eq 0 ] &&
   [ "$CHANNEL_ONE_RC" -eq 0 ] && [ "$CHANNEL_TWO_RC" -eq 0 ]; then
    tap_ok 0 "two paused channel allocations complete after busy rollback"
else
    sed 's/^/# /' "$BASE/channel-one.log" 2>/dev/null
    sed 's/^/# /' "$BASE/channel-two.log" 2>/dev/null
    tap_ok 1 "two paused channel allocations complete after busy rollback"
fi

if [ "$CHANNEL_TERM_RC" -ne 0 ] && [ "$CHANNEL_TERM_RC" -ne "$PROCESS_WAIT_TIMEOUT" ] &&
   [ "$(device_status)" = "Available" ]; then
    tap_ok 0 "CFG_TERM keeps registrations while channel callbacks are active"
else
    sed 's/^/# /' "$BASE/channel-rmdev.log" 2>/dev/null
    tap_ok 1 "CFG_TERM keeps registrations while channel callbacks are active"
fi

schedule_status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null)
case "$schedule_status" in
    *'schedule=2 reached=0xe channel_pre=2 channel_rejected=0 timed_out=0 active=0'*)
        tap_ok 0 "channel callbacks reach preadmission and busy rollback without timing out" ;;
    *)
        tap_diag "schedule status: $schedule_status"
        tap_ok 1 "channel callbacks reach preadmission and busy rollback without timing out" ;;
esac

final_failure=0
run_bounded_command "post-race device info" "$COMMAND_TIMEOUT_SECONDS" /usr/sbin/usfs_testctl info >/dev/null 2>&1 || final_failure=1
usfs_coverage_expect_export lifecycle-channel-drained-term || final_failure=1
USFS_COVERAGE_EVENT=lifecycle-channel-drained-term \
    run_bounded_command "post-race device removal" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 >/dev/null 2>&1 || final_failure=1
restore_device || final_failure=1
no_usfs_state || final_failure=1
tap_ok "$final_failure" "busy channel race leaves a usable and retryable extension"

status_failure=0
if exec 3</dev/usfs0/status; then STATUS_FD_THREE_OPEN=1; else status_failure=1; fi
if exec 4</dev/usfs0/status; then STATUS_FD_FOUR_OPEN=1; else status_failure=1; fi
run_bounded_command "two-open status termination" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 3<&- 4<&- >"$BASE/status-rmdev-two.log" 2>&1
status_term_rc=$?
if [ "$status_term_rc" -eq 0 ] || [ "$status_term_rc" -eq "$PROCESS_WAIT_TIMEOUT" ]; then status_failure=1; fi
if [ "$(device_status)" != "Available" ] ||
   ! run_bounded_command "two-open status query" "$COMMAND_TIMEOUT_SECONDS" /usr/sbin/usfsctl status 3<&- 4<&- >/dev/null 2>&1; then status_failure=1; fi
tap_ok "$status_failure" "two shared status opens keep termination busy and admission usable"

status_failure=0
if [ "$STATUS_FD_THREE_OPEN" -eq 1 ]; then exec 3<&-; STATUS_FD_THREE_OPEN=0; else status_failure=1; fi
run_bounded_command "one-open status termination" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 4<&- >"$BASE/status-rmdev-one.log" 2>&1
status_term_rc=$?
if [ "$status_term_rc" -eq 0 ] || [ "$status_term_rc" -eq "$PROCESS_WAIT_TIMEOUT" ]; then status_failure=1; fi
if [ "$(device_status)" != "Available" ] ||
   ! run_bounded_command "one-open status query" "$COMMAND_TIMEOUT_SECONDS" /usr/sbin/usfsctl status 4<&- >/dev/null 2>&1; then status_failure=1; fi
tap_ok "$status_failure" "one remaining status open still keeps termination busy"

status_failure=0
if [ "$STATUS_FD_FOUR_OPEN" -eq 1 ]; then exec 4<&-; STATUS_FD_FOUR_OPEN=0; else status_failure=1; fi
usfs_coverage_expect_export lifecycle-status-drained-term || status_failure=1
USFS_COVERAGE_EVENT=lifecycle-status-drained-term \
    run_bounded_command "drained status termination" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 >"$BASE/status-rmdev-drained.log" 2>&1 || status_failure=1
if [ "$(device_status)" != "Defined" ]; then status_failure=1; fi
restore_device || status_failure=1
tap_ok "$status_failure" "final status close permits clean termination and reconfiguration"

deallocation_failure=0
/usr/sbin/usfs_testctl schedule 5 >"$BASE/deallocation-arm.log" 2>&1 &
DEALLOCATION_ARM_PID=$!
if usfs_wait_until "deallocation schedule arm" 10 grep -q '^channel deallocation schedule armed$' "$BASE/deallocation-arm.log"; then
    tap_ok 0 "deallocation schedule arms before termination starts"

    rmdev -l usfs0 >"$BASE/deallocation-rmdev.log" 2>&1 &
    DEALLOCATION_TERM_PID=$!

    wait_owned_process "channel deallocation callback" "$DEALLOCATION_ARM_PID" "$COMMAND_TIMEOUT_SECONDS"; DEALLOCATION_ARM_RC=$?
    if process_finished "$DEALLOCATION_ARM_PID"; then DEALLOCATION_ARM_PID=0; else exit 1; fi
    wait_owned_process "deallocation-race termination" "$DEALLOCATION_TERM_PID" "$COMMAND_TIMEOUT_SECONDS"; DEALLOCATION_TERM_RC=$?
    if process_finished "$DEALLOCATION_TERM_PID"; then DEALLOCATION_TERM_PID=0; else exit 1; fi

    if [ "$DEALLOCATION_ARM_RC" -eq 0 ] && [ "$DEALLOCATION_TERM_RC" -ne 0 ] &&
       [ "$DEALLOCATION_TERM_RC" -ne "$PROCESS_WAIT_TIMEOUT" ] &&
       [ "$(device_status)" = "Available" ]; then
        tap_ok 0 "unpublished channel callback prevents runtime teardown"
    else
        sed 's/^/# /' "$BASE/deallocation-arm.log" "$BASE/deallocation-rmdev.log" 2>/dev/null
        tap_ok 1 "unpublished channel callback prevents runtime teardown"
    fi

    schedule_status=$(/usr/sbin/usfs_testctl schedule-status 2>/dev/null)
    case "$schedule_status" in
        *'schedule=5 reached=0x1006 '*'timed_out=0 active=0'*)
            ;;
        *)
            tap_diag "deallocation schedule status: $schedule_status"
            deallocation_failure=1 ;;
    esac
    usfs_coverage_expect_export lifecycle-deallocation-retry-term || deallocation_failure=1
    USFS_COVERAGE_EVENT=lifecycle-deallocation-retry-term \
        run_bounded_command "deallocation retry termination" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 >/dev/null 2>&1 || deallocation_failure=1
    restore_device || deallocation_failure=1
    tap_ok "$deallocation_failure" "deallocation race drains before successful termination retry"
else
    tap_ok 1 "deallocation schedule arms before termination starts"
    stop_owned_process "deallocation schedule arm" "$DEALLOCATION_ARM_PID" || true
    if process_finished "$DEALLOCATION_ARM_PID"; then DEALLOCATION_ARM_PID=0; else exit 1; fi
    tap_ok 1 "unpublished channel callback prevents runtime teardown"
    tap_ok 1 "deallocation race drains before successful termination retry"
fi

cycle_failure=0
cycle=0
while [ "$cycle" -lt 8 ]; do
    /usr/sbin/usfs_scenario_daemon "$MOUNT" "$SCENARIO" "$REQUESTS" \
        >"$DAEMON_LOG" 2>&1 &
    DAEMON_PID=$!
    if ! usfs_wait_until "repeated lifecycle mount" 20 is_mounted ||
       ! ls -ld "$MOUNT" >/dev/null 2>&1; then
        cycle_failure=1
    fi
    stop_daemon || cycle_failure=1
    no_usfs_state || cycle_failure=1
    cycle=$((cycle + 1))
done
usfs_coverage_checkpoint lifecycle-pre-cycle-term || cycle_failure=1
usfs_coverage_expect_export lifecycle-cycle-term || cycle_failure=1
USFS_COVERAGE_EVENT=lifecycle-cycle-term \
    run_bounded_command "cycle termination" "$COMMAND_TIMEOUT_SECONDS" rmdev -l usfs0 >/dev/null 2>&1 ||
    cycle_failure=1
restore_device || cycle_failure=1
run_bounded_command "cycle device info" "$COMMAND_TIMEOUT_SECONDS" /usr/sbin/usfs_testctl info >/dev/null 2>&1 || cycle_failure=1
tap_ok "$cycle_failure" "repeated request-close-unmount cycles permit teardown and reload"

retry_failure=0
usfs_mount_owner_case retry-descriptor 0 || retry_failure=1
no_usfs_state || retry_failure=1
tap_ok "$retry_failure" "busy descriptor preserves cleanup ownership until same-instance unmount retry"
retry_failure=0
usfs_mount_owner_case retry-mapping 0 || retry_failure=1
no_usfs_state || retry_failure=1
tap_ok "$retry_failure" "busy mapping preserves cleanup ownership until final unmap and retry"

tap_finish
