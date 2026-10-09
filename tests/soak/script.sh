#!/usr/bin/ksh
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
#
# Purpose: Run the long-running USFS reliability campaign on dedicated AIX hosts.
# Usage: ./script.sh DAYS
# Normal, resource-bounded USFS multi-day reliability campaign.

set -u

ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/lib/common.ksh"

EXIT_TEST_FAILURE=1
EXIT_PREFLIGHT=2
RUN=""
ORIGINAL_CONFIG=""
FAILURE_CAPTURED=0
CAMPAIGN_STATUS=0
TRACE_ACTIVE=0

preflight_fail()
{
    echo "PREFLIGHT: $*" >&2
    exit $EXIT_PREFLIGHT
}

event()
{
    printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >>"$RUN/events.log"
}

restore_runtime_config()
{
    [ -n "$ORIGINAL_CONFIG" ] || return 0
    _request=$(echo "$ORIGINAL_CONFIG" | awk -F= '$1 == "request_timeout_ms" {print $2}')
    _pager=$(echo "$ORIGINAL_CONFIG" | awk -F= '$1 == "pager_timeout_ms" {print $2}')
    _queue=$(echo "$ORIGINAL_CONFIG" | awk -F= '$1 == "max_outstanding_requests" {print $2}')
    [ -n "$_request" ] && [ -n "$_pager" ] && [ -n "$_queue" ] || return 1
    /usr/sbin/chusfs -l usfs0 \
        -a request_timeout_ms="$_request" \
        -a pager_timeout_ms="$_pager" \
        -a max_outstanding_requests="$_queue" >"$RUN/runtime-restored.txt" 2>&1
}

capture_failure()
{
    _reason=$1
    CAMPAIGN_STATUS=$EXIT_TEST_FAILURE
    [ "$FAILURE_CAPTURED" -eq 0 ] || return 0
    FAILURE_CAPTURED=1
    mkdir -p "$RUN/diagnostics"
    printf '%s\n' "$_reason" >"$RUN/failure-summary.txt"
    touch "$RUN/state/stop"
    event "FAILURE $_reason"
    if [ "$TRACE_ACTIVE" -eq 1 ]; then
        /usr/sbin/usfsctl trace stop >"$RUN/diagnostics/trace-stop.txt" 2>&1
        TRACE_ACTIVE=0
    fi
    soak_capture_command "$RUN/diagnostics/uname.txt" uname -a
    soak_capture_command "$RUN/diagnostics/lsdev.txt" /usr/sbin/lsdev -l usfs0
    soak_capture_command "$RUN/diagnostics/chusfs.txt" /usr/sbin/chusfs -l usfs0
    soak_capture_command "$RUN/diagnostics/usfsctl.txt" /usr/sbin/usfsctl
    soak_capture_command "$RUN/diagnostics/mount.txt" mount
    soak_capture_command "$RUN/diagnostics/processes.txt" ps -ef
    soak_capture_command "$RUN/diagnostics/vmstat.txt" vmstat 1 5
    soak_capture_command "$RUN/diagnostics/df.txt" df -Pk "$RUN"
    { echo "command: errpt"; errpt 2>&1 | sed -n '1,100p'; } >"$RUN/diagnostics/errpt.txt"
    cp "$RUN"/logs/*.log "$RUN/diagnostics/" 2>/dev/null
}

finish()
{
    _status=$CAMPAIGN_STATUS
    if [ -z "$RUN" ]; then
        trap - 0
        exit "$_status"
    fi
    if [ "$TRACE_ACTIVE" -eq 1 ]; then
        if ! /usr/sbin/usfsctl trace stop >"$RUN/diagnostics/trace-stop.txt" 2>&1; then
            [ "$_status" -ne 0 ] || _status=$EXIT_TEST_FAILURE
        fi
        TRACE_ACTIVE=0
    fi
    restore_runtime_config || {
        [ "$_status" -ne 0 ] || _status=$EXIT_TEST_FAILURE
        capture_failure "could not restore original runtime configuration"
    }
    soak_ordinary_cleanup "$RUN" || _status=$EXIT_TEST_FAILURE
    if [ "$_status" -eq 0 ]; then
        event "COMPLETE"
        echo "USFS soak completed successfully; results: $RUN"
    else
        echo "USFS soak failed; diagnostics: $RUN" >&2
    fi
    trap - 0 HUP INT TERM
    exit "$_status"
}

[ "$#" -eq 1 ] || preflight_fail "usage: ./script.sh DAYS"
soak_is_uint "$1" || preflight_fail "DAYS must be an integer from 1 to 90"
[ "$1" -ge 1 ] && [ "$1" -le 90 ] || preflight_fail "DAYS must be from 1 to 90"
[ "$(id -u)" -eq 0 ] || preflight_fail "root is required"

DURATION=$(( $1 * 86400 ))
if [ -n "${USFS_SOAK_SECONDS:-}" ]; then
    soak_is_uint "$USFS_SOAK_SECONDS" || preflight_fail "invalid USFS_SOAK_SECONDS"
    [ "$USFS_SOAK_SECONDS" -ge 30 ] && [ "$USFS_SOAK_SECONDS" -le "$DURATION" ] ||
        preflight_fail "USFS_SOAK_SECONDS must be 30..DAYS*86400"
    DURATION=$USFS_SOAK_SECONDS
fi

# Short interval overrides are strictly for the bounded archive qualification
# run. Normal operator campaigns always use the production restart cadence.
TEST_AUTOMATION=0
if [ "${USFS_SOAK_TEST_UNDERSIZED:-0}" = 1 ] &&
   [ -n "${USFS_SOAK_SECONDS:-}" ] && [ "$DURATION" -le 600 ] &&
   soak_is_uint "${USFS_SOAK_WORKERS:-}" &&
   [ "$USFS_SOAK_WORKERS" -ge 1 ] && [ "$USFS_SOAK_WORKERS" -le 2 ]; then
    TEST_AUTOMATION=1
fi

[ -r "$ROOT/manifest.txt" ] || preflight_fail "archive manifest is missing"
. "$ROOT/manifest.txt"
[ -n "${RPM_FILE:-}" ] && [ -n "${RPM_QUERY:-}" ] || preflight_fail "manifest is incomplete"
[ -f "$ROOT/$RPM_FILE" ] || preflight_fail "archive RPM is missing"
[ -f "$ROOT/CHECKSUMS.sha256" ] || preflight_fail "checksums are missing"
while read _digest _file; do
    [ -n "$_digest" ] || continue
    _file=$(echo "$_file" | sed 's/^  *//')
    [ -f "$ROOT/$_file" ] || preflight_fail "checksum file missing: $_file"
    _actual=$(cd "$ROOT" && perl bin/sha256.pl "$_file" | awk '{print $1}')
    [ "$_actual" = "$_digest" ] || preflight_fail "checksum mismatch: $_file"
done <"$ROOT/CHECKSUMS.sha256"

_installed=$(rpm -q usfs 2>/dev/null) || preflight_fail "usfs RPM is not installed"
[ "$_installed" = "$RPM_QUERY" ] ||
    preflight_fail "installed $_installed does not match archive $RPM_QUERY"
rpm -K "$ROOT/$RPM_FILE" >/dev/null 2>&1 || preflight_fail "RPM signature/digest check failed"
_verify="$ROOT/.rpm-verify.$$"
rpm -V usfs >"$_verify" 2>&1 || { rm -f "$_verify"; preflight_fail "rpm -V usfs failed"; }
[ ! -s "$_verify" ] || { sed -n '1,20p' "$_verify" >&2; rm -f "$_verify"; preflight_fail "installed RPM payload differs"; }
rm -f "$_verify"
/usr/sbin/lsdev -l usfs0 2>/dev/null | grep -q Available || preflight_fail "usfs0 is not Available"
[ -c /dev/usfs0 ] || preflight_fail "/dev/usfs0 is not a character device"

TEST_UNDERSIZED=0
CPUS=$(/usr/sbin/lsdev -Cc processor -F status 2>/dev/null |
    awk '$1 == "Available" {count += 1} END {print count + 0}')
soak_is_uint "$CPUS" || preflight_fail "cannot determine CPU count"
if [ "$CPUS" -lt 2 ]; then
    if [ "$TEST_AUTOMATION" -ne 1 ]; then
        preflight_fail "at least two CPUs are required"
    fi
    TEST_UNDERSIZED=1
    echo "NOTICE: bounded undersized-VM automation mode (not qualification)"
fi
MEMORY_KIB=$(/usr/sbin/lsattr -El sys0 -a realmem -F value 2>/dev/null | sed -n '1p')
soak_is_uint "$MEMORY_KIB" || preflight_fail "cannot determine sys0 realmem"
[ "$MEMORY_KIB" -ge 2097152 ] || preflight_fail "at least 2 GiB RAM is required"

RUN_BASE=${USFS_SOAK_WORKDIR:-$ROOT/runs}
mkdir -p "$RUN_BASE" || preflight_fail "cannot create workspace"
FREE_KIB=$(df -Pk "$RUN_BASE" | awk 'NR == 2 {print $4}')
soak_is_uint "$FREE_KIB" || preflight_fail "cannot determine workspace free space"
MIN_INITIAL_FREE_KIB=2097152
MIN_RUNTIME_FREE_KIB=1048576
MAX_RUN_KIB=524288
if [ "$TEST_UNDERSIZED" -eq 1 ]; then
    MIN_INITIAL_FREE_KIB=524288
    MIN_RUNTIME_FREE_KIB=262144
    MAX_RUN_KIB=131072
fi
[ "$FREE_KIB" -ge "$MIN_INITIAL_FREE_KIB" ] ||
    preflight_fail "workspace does not meet the required free-space reserve"

RUN="$RUN_BASE/run-$(date '+%Y%m%d-%H%M%S')-$$"
mkdir -p "$RUN/state/daemons" "$RUN/state/workers" "$RUN/logs" \
    "$RUN/diagnostics" \
    "$RUN/mounts" "$RUN/fixture" "$RUN/hostile" || preflight_fail "cannot create run directory"
: >"$RUN/events.log"
trap 'capture_failure "campaign interrupted"; exit 1' HUP INT TERM
trap finish 0

if /usr/sbin/usfsctl trace start --profile requests \
    --output "$RUN/diagnostics/usfs.trc" >"$RUN/diagnostics/trace-start.txt" 2>&1; then
    TRACE_ACTIVE=1
else
    capture_failure "could not start the owned AIX trace session"
    exit 1
fi

MEMFS_COUNT=$(soak_clamp $((CPUS / 2)) 2 4)
MIRROR_COUNT=1
[ "$CPUS" -ge 4 ] && MIRROR_COUNT=2
WORKER_COUNT=$(soak_clamp $((CPUS * 2)) 6 24)
MEMFS_MIB=$(( MEMORY_KIB / 1024 / (16 * MEMFS_COUNT) ))
MEMFS_MIB=$(soak_clamp "$MEMFS_MIB" 16 64)
MEMFS_INODES=10000
WORKER_DUTY=60
WORKER_BATCH=16
WORKER_MAX_FILES=64
RESTART_INTERVAL_SECONDS=1800

# Overrides may only reduce resource consumption, never raise safety ceilings.
if [ -n "${USFS_SOAK_WORKERS:-}" ]; then
    soak_is_uint "$USFS_SOAK_WORKERS" || preflight_fail "invalid USFS_SOAK_WORKERS"
    [ "$USFS_SOAK_WORKERS" -ge 1 ] && [ "$USFS_SOAK_WORKERS" -le "$WORKER_COUNT" ] || preflight_fail "worker override exceeds safe computed limit"
    WORKER_COUNT=$USFS_SOAK_WORKERS
fi
if [ -n "${USFS_SOAK_MEMFS_MIB:-}" ]; then
    soak_is_uint "$USFS_SOAK_MEMFS_MIB" || preflight_fail "invalid USFS_SOAK_MEMFS_MIB"
    [ "$USFS_SOAK_MEMFS_MIB" -ge 4 ] && [ "$USFS_SOAK_MEMFS_MIB" -le "$MEMFS_MIB" ] || preflight_fail "memfs override exceeds safe computed limit"
    MEMFS_MIB=$USFS_SOAK_MEMFS_MIB
fi
if [ -n "${USFS_SOAK_RESTART_SECONDS:-}" ]; then
    [ "$TEST_AUTOMATION" -eq 1 ] ||
        preflight_fail "restart interval override is restricted to bounded automation"
    soak_is_uint "$USFS_SOAK_RESTART_SECONDS" ||
        preflight_fail "invalid USFS_SOAK_RESTART_SECONDS"
    [ "$USFS_SOAK_RESTART_SECONDS" -ge 30 ] &&
        [ "$USFS_SOAK_RESTART_SECONDS" -le $((DURATION - 30)) ] ||
        preflight_fail "USFS_SOAK_RESTART_SECONDS must leave 30 seconds after restart"
    RESTART_INTERVAL_SECONDS=$USFS_SOAK_RESTART_SECONDS
fi

ORIGINAL_CONFIG=$(/usr/sbin/chusfs -l usfs0) || preflight_fail "cannot read runtime configuration"
printf '%s\n' "$ORIGINAL_CONFIG" >"$RUN/runtime-original.txt"
{
    echo "start=$(date '+%Y-%m-%dT%H:%M:%S')"
    echo "duration_seconds=$DURATION"
    echo "cpus=$CPUS"
    echo "memory_kib=$MEMORY_KIB"
    echo "memfs_count=$MEMFS_COUNT"
    echo "mirror_count=$MIRROR_COUNT"
    echo "worker_count=$WORKER_COUNT"
    echo "memfs_mib=$MEMFS_MIB"
    echo "memfs_inodes=$MEMFS_INODES"
    echo "restart_interval_seconds=$RESTART_INTERVAL_SECONDS"
    echo "archive_revision=${BUILD_REVISION:-unknown}"
} >"$RUN/campaign.conf"

_fixture_index=0
while [ "$_fixture_index" -lt 128 ]; do
    printf 'USFS bounded mirror fixture %03d\n' "$_fixture_index" >"$RUN/fixture/file$(printf '%03d' "$_fixture_index")"
    _fixture_index=$((_fixture_index + 1))
done
mkdir "$RUN/fixture/tree"
ln -s ../file000 "$RUN/fixture/tree/link"

start_daemon()
{
    _id=$1
    _type=$2
    _mount="$RUN/mounts/$_id"
    mkdir -p "$_mount"
    case "$_type" in
        memfs) "$ROOT/bin/usfs_memfs" --size="$MEMFS_MIB" --inodes="$MEMFS_INODES" "$_mount" ;;
        mirror) "$ROOT/bin/usfs_mirror" --source="$RUN/fixture" "$_mount" ;;
        *) return 2 ;;
    esac </dev/null >>"$RUN/logs/$_id.log" 2>&1 &
    _pid=$!
    echo "$_pid" >"$RUN/state/daemons/$_id.pid"
    echo "$_mount" >"$RUN/state/daemons/$_id.mount"
    echo "$_type" >"$RUN/state/daemons/$_id.type"
    soak_wait_mounted "$_mount" 60 || return 1
    event "DAEMON_START id=$_id type=$_type pid=$_pid mount=$_mount"
}

_index=1
while [ "$_index" -le "$MEMFS_COUNT" ]; do
    start_daemon "memfs$_index" memfs || { capture_failure "memfs$_index did not mount"; exit 1; }
    _index=$((_index + 1))
done
_index=1
while [ "$_index" -le "$MIRROR_COUNT" ]; do
    start_daemon "mirror$_index" mirror || { capture_failure "mirror$_index did not mount"; exit 1; }
    _index=$((_index + 1))
done
NORMAL_MOUNTS=$((MEMFS_COUNT + MIRROR_COUNT))
[ "$NORMAL_MOUNTS" -lt 9 ] || { capture_failure "computed topology exceeds channel ceiling"; exit 1; }
CAMPAIGN_SEED=$(date +%s)
_worker=1
while [ "$_worker" -le "$WORKER_COUNT" ]; do
    _slot=$(( (_worker - 1) % NORMAL_MOUNTS + 1 ))
    if [ "$_slot" -le "$MEMFS_COUNT" ]; then
        _daemon="memfs$_slot"
        _mode=mutable
    elif [ "$_slot" -le $((MEMFS_COUNT + MIRROR_COUNT)) ]; then
        _daemon="mirror$((_slot - MEMFS_COUNT))"
        _mode=readonly
    fi
    _mount=$(sed -n '1p' "$RUN/state/daemons/$_daemon.mount")
    _seed=$((CAMPAIGN_SEED * 100 + _worker))
    _status="$RUN/state/workers/worker$_worker.status"
    _pause="$RUN/state/daemons/$_daemon.pause"
    _fail_args=""
    if [ -n "${USFS_SOAK_INJECT_WORKER_FAILURE:-}" ] && [ "$_worker" -eq 1 ]; then
        _fail_args="--fail-after ${USFS_SOAK_INJECT_WORKER_FAILURE}"
    fi
    "$ROOT/bin/usfs_soak_worker" --root "$_mount" --mode "$_mode" \
        --seed "$_seed" --status "$_status" --stop "$RUN/state/stop" \
        --pause "$_pause" --duty "$WORKER_DUTY" --batch "$WORKER_BATCH" \
        --max-files "$WORKER_MAX_FILES" $_fail_args \
        >>"$RUN/logs/worker$_worker.log" 2>&1 &
    _pid=$!
    echo "$_pid" >"$RUN/state/workers/worker$_worker.pid"
    echo "$_daemon" >"$RUN/state/workers/worker$_worker.daemon"
    echo "$_seed" >"$RUN/state/workers/worker$_worker.seed"
    event "WORKER_START id=$_worker daemon=$_daemon pid=$_pid seed=$_seed mode=$_mode"
    _worker=$((_worker + 1))
done

run_hostile()
{
    _case=$1
    _seed=$2
    _mount="$RUN/hostile/mount"
    _scenario="$ROOT/scenarios/$_case.scn"
    _request_log="$RUN/hostile/$_case-$_seed.requests"
    _daemon_log="$RUN/logs/hostile.log"
    rmdir "$_mount" 2>/dev/null
    mkdir -p "$_mount"
    printf 'case=%s seed=%s timestamp=%s\n' "$_case" "$_seed" "$(date +%s)" >"$RUN/hostile/current"
    "$ROOT/bin/usfs_scenario_daemon" "$_mount" "$_scenario" "$_request_log" \
        </dev/null >>"$_daemon_log" 2>&1 &
    _hostile_pid=$!
    if ! soak_wait_mounted "$_mount" 30; then
        soak_bounded_kill "$_hostile_pid" TERM
        return 1
    fi
    case "$_case" in
        malformed-padding|malformed-count) soak_run_with_deadline 20 cat "$_mount/file" >/dev/null 2>&1 ;;
        malformed-offset) soak_run_with_deadline 20 /usr/bin/ksh -c "echo x >'$_mount/file'" >/dev/null 2>&1 ;;
        reordered)
            ls "$_mount/file" >/dev/null 2>&1 & _first=$!
            ls "$_mount/dir" >/dev/null 2>&1 & _second=$!
            soak_wait_pid_gone "$_first" 20 || kill -TERM "$_first" 2>/dev/null
            soak_wait_pid_gone "$_second" 20 || kill -TERM "$_second" 2>/dev/null
            ;;
        *) soak_run_with_deadline 20 ls "$_mount/file" >/dev/null 2>&1 ;;
    esac
    if soak_is_mounted "$_mount"; then
        soak_run_with_deadline 30 umount "$_mount" >/dev/null 2>&1 || {
            soak_bounded_kill "$_hostile_pid" TERM
            return 1
        }
    fi
    soak_bounded_kill "$_hostile_pid" TERM
    soak_is_mounted "$_mount" && return 1
    rm -f "$_request_log"

    "$ROOT/bin/usfs_scenario_daemon" "$_mount" "$ROOT/scenarios/clean.scn" \
        "$RUN/hostile/recovery.requests" </dev/null >>"$_daemon_log" 2>&1 &
    _clean_pid=$!
    soak_wait_mounted "$_mount" 30 || { soak_bounded_kill "$_clean_pid" TERM; return 1; }
    soak_run_with_deadline 20 cat "$_mount/file" >/dev/null 2>&1 || {
        soak_run_with_deadline 30 umount "$_mount" >/dev/null 2>&1
        soak_bounded_kill "$_clean_pid" TERM
        return 1
    }
    soak_run_with_deadline 30 umount "$_mount" >/dev/null 2>&1 || return 1
    soak_bounded_kill "$_clean_pid" TERM || return 1
    rm -f "$RUN/hostile/recovery.requests" "$RUN/hostile/current"
    event "HOSTILE_RECOVERED case=$_case seed=$_seed"
    return 0
}

wait_workers_paused()
{
    _daemon=$1
    _deadline=$(( $(date +%s) + 90 ))
    while [ "$(date +%s)" -lt "$_deadline" ]; do
        _all=1
        for _daemon_file in "$RUN"/state/workers/*.daemon; do
            [ "$(sed -n '1p' "$_daemon_file")" = "$_daemon" ] || continue
            _status=${_daemon_file%.daemon}.status
            grep -q '^state=paused$' "$_status" 2>/dev/null || _all=0
        done
        [ "$_all" -eq 1 ] && return 0
        sleep 1
    done
    return 1
}

restart_daemon()
{
    _id=$1
    _style=$2
    _pid=$(sed -n '1p' "$RUN/state/daemons/$_id.pid")
    _mount=$(sed -n '1p' "$RUN/state/daemons/$_id.mount")
    _type=$(sed -n '1p' "$RUN/state/daemons/$_id.type")
    touch "$RUN/state/daemons/$_id.pause"
    wait_workers_paused "$_id" || return 1
    if [ "$_style" = abrupt ]; then
        soak_bounded_kill "$_pid" KILL || return 1
        soak_is_mounted "$_mount" &&
            soak_run_with_deadline 30 umount "$_mount" >/dev/null 2>&1 || :
    else
        if soak_is_mounted "$_mount"; then
            soak_run_with_deadline 30 umount "$_mount" >/dev/null 2>&1 || return 1
        fi
        soak_bounded_kill "$_pid" TERM || return 1
    fi
    soak_is_mounted "$_mount" && return 1
    start_daemon "$_id" "$_type" || return 1
    rm -f "$RUN/state/daemons/$_id.pause"
    event "DAEMON_RESTART id=$_id style=$_style"
}

HOSTILE_CASES="malformed-version malformed-opcode malformed-unique malformed-length malformed-body malformed-padding malformed-count malformed-offset duplicate reordered delayed unsolicited random-bytes disconnect"
HOSTILE_INDEX=0
START_TIME=$(date +%s)
END_TIME=$((START_TIME + DURATION))
NEXT_MONITOR=$START_TIME
NEXT_HOSTILE=$START_TIME
NEXT_ADMIN=$((START_TIME + 10))
NEXT_RESTART=$((START_TIME + RESTART_INTERVAL_SECONDS))
NEXT_HEARTBEAT=$((START_TIME + 900))
RESTART_NUMBER=0

event "CAMPAIGN_START duration=$DURATION"
while [ "$(date +%s)" -lt "$END_TIME" ]; do
    NOW=$(date +%s)
    if [ "$NOW" -ge "$NEXT_HOSTILE" ]; then
        _case_index=0
        _selected=""
        for _case in $HOSTILE_CASES; do
            [ "$_case_index" -eq $((HOSTILE_INDEX % 14)) ] && _selected=$_case
            _case_index=$((_case_index + 1))
        done
        run_hostile "$_selected" "$((CAMPAIGN_SEED + HOSTILE_INDEX))" || {
            capture_failure "hostile case $_selected did not recover through a clean channel"
            exit 1
        }
        HOSTILE_INDEX=$((HOSTILE_INDEX + 1))
        NEXT_HOSTILE=$((NOW + 30))
    fi
    if [ "$NOW" -ge "$NEXT_ADMIN" ]; then
        "$ROOT/bin/usfs_admin_probe" --safe >>"$RUN/logs/admin.log" 2>&1 || {
            capture_failure "public administrative probe failed"
            exit 1
        }
        su nobody -c "$ROOT/bin/usfs_admin_probe --unprivileged" \
            >>"$RUN/logs/admin.log" 2>&1 || {
            capture_failure "unprivileged administrative denial probe failed"
            exit 1
        }
        if [ $((HOSTILE_INDEX % 2)) -eq 0 ]; then
            _request=45000; _pager=15000; _queue=128
        else
            _request=30000; _pager=10000; _queue=256
        fi
        /usr/sbin/chusfs -l usfs0 -a request_timeout_ms="$_request" \
            -a pager_timeout_ms="$_pager" -a max_outstanding_requests="$_queue" \
            >>"$RUN/logs/admin.log" 2>&1 || {
            capture_failure "safe runtime policy toggle failed"
            exit 1
        }
        event "ADMIN_PROBE request=$_request pager=$_pager queue=$_queue"
        NEXT_ADMIN=$((NOW + 600))
    fi
    if [ "$NOW" -ge "$NEXT_RESTART" ]; then
        _target="memfs$((RESTART_NUMBER % MEMFS_COUNT + 1))"
        _style=graceful
        [ $((RESTART_NUMBER % 2)) -eq 1 ] && _style=abrupt
        restart_daemon "$_target" "$_style" || {
            capture_failure "coordinated $_style restart failed for $_target"
            exit 1
        }
        RESTART_NUMBER=$((RESTART_NUMBER + 1))
        NEXT_RESTART=$((NOW + RESTART_INTERVAL_SECONDS))
    fi
    if [ "$NOW" -ge "$NEXT_MONITOR" ]; then
        for _pid_file in "$RUN"/state/daemons/*.pid "$RUN"/state/workers/*.pid; do
            [ -f "$_pid_file" ] || continue
            _pid=$(sed -n '1p' "$_pid_file")
            kill -0 "$_pid" 2>/dev/null || {
                capture_failure "unexpected process exit: $_pid_file pid=$_pid"
                exit 1
            }
        done
        for _mount_file in "$RUN"/state/daemons/*.mount; do
            _mount=$(sed -n '1p' "$_mount_file")
            soak_is_mounted "$_mount" || {
                capture_failure "mount disappeared: $_mount"
                exit 1
            }
        done
        for _status in "$RUN"/state/workers/*.status; do
            [ -f "$_status" ] || { capture_failure "worker status missing: $_status"; exit 1; }
            grep -q '^state=failed$' "$_status" && { capture_failure "worker reported failure: $_status"; exit 1; }
            _timestamp=$(awk -F= '$1 == "timestamp" {print $2}' "$_status")
            soak_is_uint "$_timestamp" || { capture_failure "invalid worker status: $_status"; exit 1; }
            [ $((NOW - _timestamp)) -le 300 ] || { capture_failure "five minutes without worker progress: $_status"; exit 1; }
        done
        FREE_KIB=$(df -Pk "$RUN" | awk 'NR == 2 {print $4}')
        # Count the host workspace once, without traversing live USFS mounts.
        # Those trees are being renamed/removed and mirror data is already
        # counted through the backing fixture. Never accept a partial du sum.
        _usage=$(du -skx "$RUN") || { capture_failure "workspace usage measurement failed"; exit 1; }
        USED_KIB=$(printf '%s\n' "$_usage" | awk 'NR == 1 {print $1}')
        soak_is_uint "$FREE_KIB" && soak_is_uint "$USED_KIB" || {
            capture_failure "invalid workspace usage measurement"
            exit 1
        }
        [ "$FREE_KIB" -ge "$MIN_RUNTIME_FREE_KIB" ] || { capture_failure "workspace free space fell below its reserved minimum"; exit 1; }
        [ "$USED_KIB" -le "$MAX_RUN_KIB" ] || { capture_failure "campaign workspace exceeded its bounded maximum"; exit 1; }
        /usr/sbin/chusfs -l usfs0 >"$RUN/state/runtime.status" 2>&1 || { capture_failure "runtime policy monitor failed"; exit 1; }
        NEXT_MONITOR=$((NOW + 60))
    fi
    if [ "$NOW" -ge "$NEXT_HEARTBEAT" ]; then
        event "HEARTBEAT elapsed=$((NOW - START_TIME)) free_kib=$FREE_KIB"
        NEXT_HEARTBEAT=$((NOW + 900))
    fi
    sleep 2
done

touch "$RUN/state/stop"
exit 0
